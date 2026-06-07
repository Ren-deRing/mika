/*
 * Mika Kernel Low Level Memory Management Logic
 *
 * Copyright (c) 2026 Ren-deRing (JONGHYUN WON)
 * 
 * SPDX-License-Identifier: 0BSD
 */

#include <stdbool.h>

#include <string.h>

#include <kernel/printf.h>
#include <kernel/init.h>
#include <kernel/cpu.h>
#include <kernel/mmu.h>
#include <kernel/sched.h>
#include <kernel/lock.h>
#include <kernel/intc.h>

static spinlock_t g_mmu_lock = SPINLOCK_INITIALIZER;

void handle_page_fault(struct trapframe *regs, void *data);

#include <boot/bootinfo.h>

#include <uapi/elf.h>

#define X86_PTE_PRESENT  (1ULL << 0)
#define X86_PTE_WRITABLE (1ULL << 1)
#define X86_PTE_USER     (1ULL << 2)
#define X86_PTE_PWT      (1ULL << 3)
#define X86_PTE_PCD      (1ULL << 4)
#define X86_PTE_ACCESSED (1ULL << 5)
#define X86_PTE_DIRTY    (1ULL << 6)
#define X86_PTE_HUGE     (1ULL << 7)
#define X86_PTE_GLOBAL   (1ULL << 8)
#define X86_PTE_NX       (1ULL << 63)
#define X86_PTE_COW      (1ULL << 9)
#define X86_PTE_SHARED   (1ULL << 10)
#define X86_PTE_DEMAND   (1ULL << 11)

#define PAGE_ADDR_MASK   0x000ffffffffff000ull

#define GET_PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define GET_PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define GET_PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define GET_PT_IDX(v)   (((v) >> 12) & 0x1FF)

#define ENTRY_TO_VIRT(e) (p2v((e) & PAGE_ADDR_MASK))

typedef struct {
    page_t* free_lists[MAX_ORDER];
    page_t* all_pages_array;
    uint64_t total_pages;
    uint64_t last_phys_addr;
} pmm_t;

typedef uint64_t pt_entry_t;

struct page_table {
    pt_entry_t entries[512];
};

page_t* pmm_alloc_pages(uint8_t order);
void pmm_free_pages(page_t* page, uint8_t order);

void add_to_list(page_t** head, page_t* node);
void remove_from_list(page_t** head, page_t* target);

page_t* phys_to_page(uint64_t phys);
uint64_t page_to_phys(page_t* page);
page_t* get_buddy(page_t* page, uint8_t order);

bool vmm_map(page_table_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(page_table_t* pml4, uint64_t virt);
pt_entry_t* vmm_get_pte(page_table_t* pml4, uint64_t virt, bool allocate);
page_table_t* vmm_get_current_pml4(void);
uint64_t vmm_get_phys(page_table_t* pml4, uint64_t virt);

pmm_t g_pmm;

void pmm_init(void) {
    MemoryRegion* mmap = g_boot_info.mmap;

    if (mmap == NULL) {
        return;
    }

    uint64_t entry_count = g_boot_info.mmap_entries;

    uint64_t max_usable_addr = 0;
    for (uint64_t i = 0; i < g_boot_info.mmap_entries; i++) {
        if (mmap[i].type == MMAP_FREE || 
            mmap[i].type == MMAP_KERNEL_AND_MODULES || 
            mmap[i].type == MMAP_BOOTLOADER_RECLAIM) {
            
            uint64_t end = mmap[i].base + mmap[i].length;
            if (end > max_usable_addr) {
                max_usable_addr = end;
            }
        }
    }
    g_pmm.total_pages = ALIGN_UP(max_usable_addr, PAGE_SIZE) / PAGE_SIZE;
    uint64_t array_size = ALIGN_UP(g_pmm.total_pages * sizeof(page_t), PAGE_SIZE);

    uint64_t array_phys_addr = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        MemoryRegion *entry = &mmap[i];
        if (entry->type == MMAP_FREE) {
            uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
            uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);
            if (end > start && (end - start) >= array_size) {
                array_phys_addr = start;
                break;
            }
        }
    }

    if (array_phys_addr == 0) return; 

    g_pmm.all_pages_array = (page_t*)(p2v(array_phys_addr));
    memset(g_pmm.all_pages_array, 0, array_size);

    uint64_t array_end = array_phys_addr + array_size;

    for (uint64_t i = 0; i < entry_count; i++) {
        MemoryRegion *entry = &mmap[i];
        if (entry->type != MMAP_FREE) continue;

        uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);

        for (uint64_t curr = start; curr < end; ) {
            if (curr >= array_phys_addr && curr < array_end) {
                curr = array_end;
                continue;
            }

            uint64_t limit = end;
            if (curr < array_phys_addr && end > array_phys_addr) {
                limit = array_phys_addr;
            }

            uint64_t remaining = limit - curr;
            uint8_t order = 0;

            for (int o = MAX_ORDER - 1; o >= 0; o--) {
                uint64_t block_size = (uint64_t)PAGE_SIZE << o;
                if (remaining >= block_size && (curr % block_size) == 0) {
                    order = o;
                    break;
                }
            }

            page_t* page = phys_to_page(curr);
            page->order = order;
            page->is_free = true;
            add_to_list(&g_pmm.free_lists[order], page);

            curr += ((uint64_t)PAGE_SIZE << order);
        }
    }
}

page_t* pmm_alloc_pages(uint8_t order) {
    uint8_t current_order;
    page_t* page = NULL;

    // 충분히 크지만 가장 작은 블록을 찾음
    for (current_order = order; current_order < MAX_ORDER; current_order++) {
        if (g_pmm.free_lists[current_order] != NULL) {
            page = g_pmm.free_lists[current_order];
            remove_from_list(&g_pmm.free_lists[current_order], page);
            break;
        }
    }

    if (page == NULL) {
        dprintf("PMM: Out of memory at order %d\n", order);
        return NULL; // 맞는 블록이 없음!!
    }

    // 블록을 맞는 형태까지 자름
    while (current_order > order) {
        current_order--;
        
        // 다른 반은 우리 Buddy
        page_t* buddy = get_buddy(page, current_order);
        
        // Buddy로 추가하고 free list에 추가
        buddy->is_free = true;
        buddy->order = current_order;
        add_to_list(&g_pmm.free_lists[current_order], buddy);
    }

    page->is_free = false;
    page->order = order;
    
    return page;
}

void pmm_free_pages(page_t* page, uint8_t order) {
    page->is_free = true;
    page->order = order;

    // buddy들과 합병 시도
    while (order < MAX_ORDER - 1) {
        page_t* buddy = get_buddy(page, order);

        // Buddy가 준비됐나요?
        if (buddy < g_pmm.all_pages_array ||
            buddy >= g_pmm.all_pages_array + g_pmm.total_pages ||
            !buddy->is_free ||
            buddy->order != order) {
            break; // 아니요?
        }

        // 합병 가능하므로 free list에서 삭제
        remove_from_list(&g_pmm.free_lists[order], buddy);

        // 새 큰 블록 생성
        if (buddy < page) {
            page = buddy;
        }

        // order를 키워 다음 단계 반복
        order++;
        page->order = order;
    }

    // 모두 합병된 블록 free list에 추가
    add_to_list(&g_pmm.free_lists[order], page);
}

void add_to_list(page_t** head, page_t* node) {
    node->next = *head;
    node->prev = NULL;
    if (*head != NULL) {
        (*head)->prev = node;
    }
    *head = node;
}

void remove_from_list(page_t** head, page_t* target) {
    if (!head || !target) return;
    
    page_t* next = target->next;
    page_t* prev = target->prev;

    if (prev != NULL) {
        prev->next = next;
    } else {
        *head = next;
    }

    if (next != NULL) {
        next->prev = prev;
    }

    target->next = NULL;
    target->prev = NULL;
}

uint64_t page_to_phys(page_t* page) {
    if (!page) return 0;
    uint64_t idx = page - g_pmm.all_pages_array;
    return idx * PAGE_SIZE; 
}

page_t* phys_to_page(uint64_t phys) {
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= g_pmm.total_pages) return NULL;
    return &g_pmm.all_pages_array[idx];
}

page_t* get_buddy(page_t* page, uint8_t order) {
    uint64_t phys = page_to_phys(page);
    uint64_t buddy_phys = phys ^ ((uint64_t)PAGE_SIZE << order);
    return phys_to_page(buddy_phys);
}

page_table_t* g_kernel_pagemap = NULL;

static inline void set_cr3(uintptr_t pml4_phys) {
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static inline uintptr_t get_cr3(void) {
    uintptr_t pml4_phys;
    asm volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    return pml4_phys;
}

static inline void invlpg(uint64_t vaddr) {
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

pt_entry_t* vmm_get_pte(page_table_t* pml4, uint64_t virt, bool allocate) {
    page_table_t* current_table = pml4;
    uint64_t indices[4] = { GET_PML4_IDX(virt), GET_PDPT_IDX(virt), GET_PD_IDX(virt), GET_PT_IDX(virt) };

    for (int i = 0; i < 3; i++) {
        pt_entry_t* entry = &current_table->entries[indices[i]];
        
        if (!(*entry & X86_PTE_PRESENT)) {
            if (!allocate) return NULL;
            page_t* new_table_page = pmm_alloc_pages(0);
            if (!new_table_page) return NULL;

            uint64_t new_table_phys = page_to_phys(new_table_page);
            memset(p2v(new_table_phys), 0, PAGE_SIZE);

            new_table_page->ref_count = 0;
            page_t* parent_page = phys_to_page(v2p(current_table));
            if (parent_page) parent_page->ref_count++;

            *entry = new_table_phys | X86_PTE_PRESENT | X86_PTE_WRITABLE | X86_PTE_USER;
        } else {
            if (allocate) {
                *entry |= X86_PTE_WRITABLE | X86_PTE_USER;
            }
        }

        if (*entry & X86_PTE_HUGE) return NULL;

        current_table = (page_table_t*)ENTRY_TO_VIRT(*entry);
    }

    return &current_table->entries[indices[3]];
}

static void vmm_free_pt(page_table_t* pt) {
    for (int i = 0; i < 512; i++) {
        if (pt->entries[i] & X86_PTE_PRESENT) {
            uint64_t phys = pt->entries[i] & PAGE_ADDR_MASK;
            page_t* page = phys_to_page(phys);
            if (page) {
                if (page->ref_count > 0) page->ref_count--;
            }
            pt->entries[i] = 0;
        }
    }
    pmm_free_pages(phys_to_page(v2p(pt)), 0);
}

bool vmm_map_huge(page_table_t* pml4, uint64_t virt, uint64_t phys, uint64_t x86_flags) {
    if ((virt & 0x1FFFFF) != 0 || (phys & 0x1FFFFF) != 0) return false;

    page_table_t* current_table = pml4;
    uint64_t indices[2] = { GET_PML4_IDX(virt), GET_PDPT_IDX(virt) };

    for (int i = 0; i < 2; i++) {
        pt_entry_t* entry = &current_table->entries[indices[i]];
        if (!(*entry & X86_PTE_PRESENT)) {
            page_t* new_table_page = pmm_alloc_pages(0);
            if (!new_table_page) return false;
            
            uint64_t new_table_phys = page_to_phys(new_table_page);
            memset(p2v(new_table_phys), 0, PAGE_SIZE);
            
            new_table_page->ref_count = 0;
            page_t* parent_page = phys_to_page(v2p(current_table));
            if (parent_page) parent_page->ref_count++;

            *entry = new_table_phys | X86_PTE_PRESENT | X86_PTE_WRITABLE | X86_PTE_USER;
        }
        current_table = (page_table_t*)ENTRY_TO_VIRT(*entry);
    }

    uint64_t pd_idx = GET_PD_IDX(virt);
    pt_entry_t* entry = &current_table->entries[pd_idx];
    
    if (*entry & X86_PTE_PRESENT) {
        if (!(*entry & X86_PTE_HUGE)) {
            page_table_t* old_pt = (page_table_t*)ENTRY_TO_VIRT(*entry);
            vmm_free_pt(old_pt);
        } else {
            page_t* old_huge_page = phys_to_page(*entry & PAGE_ADDR_MASK);
            if (old_huge_page && old_huge_page->ref_count > 0) old_huge_page->ref_count--;
        }
    } else {
        page_t* pd_page = phys_to_page(v2p(current_table));
        if (pd_page) pd_page->ref_count++;
    }

    *entry = (phys & PAGE_ADDR_MASK) | x86_flags | X86_PTE_HUGE;
    page_t* new_page = phys_to_page(phys);
    if (new_page) new_page->ref_count++;

    invlpg(virt);
    return true;
}

bool vmm_map(page_table_t* pml4, uint64_t virt, uint64_t phys, uint64_t x86_flags) {
    pt_entry_t* pte = vmm_get_pte(pml4, virt, true);
    if (!pte) return false;

    if (!(*pte & X86_PTE_PRESENT)) {
        uintptr_t pt_vaddr = ALIGN_DOWN((uintptr_t)pte, PAGE_SIZE);
        page_t* pt_page = phys_to_page(v2p((void*)pt_vaddr));
        if (pt_page) pt_page->ref_count++;
    } else {
        page_t* old_page = phys_to_page(*pte & PAGE_ADDR_MASK);
        if (old_page && old_page->ref_count > 0) old_page->ref_count--;
    }

    *pte = (phys & PAGE_ADDR_MASK) | x86_flags;
    page_t* new_page = phys_to_page(phys);
    if (new_page) new_page->ref_count++;

    invlpg(virt);
    return true;
}

void vmm_init(void) {
    MemoryRegion* mmap = g_boot_info.mmap;

    page_t* pml4_page = pmm_alloc_pages(0);
    if (!pml4_page) {
        dprintf("PANIC: Failed to allocate PML4\n");
        while(1);
    }
    
    g_kernel_pagemap = (page_table_t*)p2v(page_to_phys(pml4_page));
    memset(g_kernel_pagemap, 0, PAGE_SIZE);
    pml4_page->ref_count = 1;

    for (uint64_t i = 0; i < g_boot_info.mmap_entries; i++) {
        MemoryRegion *entry = &mmap[i];
        
        uint64_t base = ALIGN_UP(entry->base, PAGE_SIZE_2M);
        uint64_t top = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE_2M);

        for (uint64_t phys = base; phys < top; phys += PAGE_SIZE_2M) {
            uint64_t virt = (uint64_t)p2v(phys);
            vmm_map_huge(g_kernel_pagemap, virt, phys, X86_PTE_PRESENT | X86_PTE_WRITABLE);
        }

        for (uint64_t phys = ALIGN_UP(entry->base, PAGE_SIZE); phys < base; phys += PAGE_SIZE) {
            vmm_map(g_kernel_pagemap, (uint64_t)p2v(phys), phys, X86_PTE_PRESENT | X86_PTE_WRITABLE);
        }
        for (uint64_t phys = top; phys < ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE); phys += PAGE_SIZE) {
            vmm_map(g_kernel_pagemap, (uint64_t)p2v(phys), phys, X86_PTE_PRESENT | X86_PTE_WRITABLE);
        }
    }

    uint64_t kfile_ptr = (uintptr_t)g_boot_info.kernel.file_ptr;
    uint64_t kvirt_base = g_boot_info.kernel.virt_base;
    uint64_t kphys_base = g_boot_info.kernel.phys_base;

    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)kfile_ptr;
    Elf64_Phdr* phdr = (Elf64_Phdr*)(kfile_ptr + ehdr->e_phoff);

    for (size_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uint64_t virt_start = ALIGN_DOWN(phdr[i].p_vaddr, PAGE_SIZE);
        uint64_t phys_start = (virt_start - kvirt_base) + kphys_base;
        uint64_t size = ALIGN_UP(phdr[i].p_memsz + (phdr[i].p_vaddr % PAGE_SIZE), PAGE_SIZE);

        uint64_t flags = X86_PTE_PRESENT;
        if (phdr[i].p_flags & PF_W) flags |= X86_PTE_WRITABLE;
        if (!(phdr[i].p_flags & PF_X)) flags |= X86_PTE_NX;

        for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
            vmm_map(g_kernel_pagemap, virt_start + offset, phys_start + offset, flags);
        }
    }

    // TODO
    for (int i = 256; i < 512; i++) {
        if (!(g_kernel_pagemap->entries[i] & X86_PTE_PRESENT)) {
            page_t* new_table_page = pmm_alloc_pages(0);
            if (!new_table_page) {
                continue; 
            }

            uint64_t new_table_phys = page_to_phys(new_table_page);
            memset(p2v(new_table_phys), 0, PAGE_SIZE);

            g_kernel_pagemap->entries[i] = new_table_phys | X86_PTE_PRESENT | X86_PTE_WRITABLE;
            
            new_table_page->ref_count = 1; 
        }
    }

    set_cr3(v2p(g_kernel_pagemap));

    extern void register_handler(uint8_t vector, handler_t handler, void *data);
    register_handler(14, handle_page_fault, NULL);
}

void vmm_unmap(page_table_t* pml4, uint64_t virt) {
    uint64_t idxs[4] = { GET_PML4_IDX(virt), GET_PDPT_IDX(virt), GET_PD_IDX(virt), GET_PT_IDX(virt) };
    pt_entry_t* entries[4];
    page_table_t* tables[4];

    tables[0] = pml4;
    for (int i = 0; i < 2; i++) {
        entries[i] = &tables[i]->entries[idxs[i]];
        if (!(*entries[i] & X86_PTE_PRESENT)) return;
        tables[i+1] = (page_table_t*)ENTRY_TO_VIRT(*entries[i]);
    }

    entries[2] = &tables[2]->entries[idxs[2]];
    if (!(*entries[2] & X86_PTE_PRESENT)) return;

    if (*entries[2] & X86_PTE_HUGE) {
        uint64_t phys = *entries[2] & PAGE_ADDR_MASK;
        page_t* page = phys_to_page(phys);
        if (page && page->ref_count > 0) {
            page->ref_count--;
            if (page->ref_count == 0) {
                page_free(page, 9);
            }
        }

        *entries[2] = 0;
        invlpg(virt);

        for (int i = 2; i > 0; i--) {
            page_t* table_page = phys_to_page(v2p(tables[i]));
            if (--table_page->ref_count == 0) {
                *entries[i-1] = 0;
                pmm_free_pages(table_page, 0);
            } else break;
        }
        return;
    }

    tables[3] = (page_table_t*)ENTRY_TO_VIRT(*entries[2]);
    entries[3] = &tables[3]->entries[idxs[3]];
    if (!(*entries[3] & X86_PTE_PRESENT) && !(*entries[3] & X86_PTE_DEMAND)) return;

    uint64_t phys = *entries[3] & PAGE_ADDR_MASK;
    page_t* page = phys_to_page(phys);
    if (page && (*entries[3] & X86_PTE_PRESENT) && page->ref_count > 0) {
        page->ref_count--;
        if (page->ref_count == 0) {
            page_free(page, 0);
        }
    }

    *entries[3] = 0;
    invlpg(virt);

    for (int i = 3; i > 0; i--) {
        page_t* table_page = phys_to_page(v2p(tables[i]));
        if (--table_page->ref_count == 0) {
            *entries[i-1] = 0;
            pmm_free_pages(table_page, 0);
        } else break;
    }
}

page_table_t* vmm_get_current_pml4(void) {
    return (page_table_t*)p2v(get_cr3());
}

uint64_t vmm_get_phys(page_table_t* pml4, uint64_t virt) {
    if (!pml4) return 0;
    page_table_t* current_table = pml4;
    uint64_t indices[4] = { GET_PML4_IDX(virt), GET_PDPT_IDX(virt), GET_PD_IDX(virt), GET_PT_IDX(virt) };

    for (int i = 0; i < 2; i++) {
        pt_entry_t entry = current_table->entries[indices[i]];
        if (!(entry & X86_PTE_PRESENT)) return 0;
        current_table = (page_table_t*)ENTRY_TO_VIRT(entry);
    }

    pt_entry_t pd_entry = current_table->entries[indices[2]];
    if (!(pd_entry & X86_PTE_PRESENT)) return 0;

    // Huge Page(
    if (pd_entry & X86_PTE_HUGE) {
        return (pd_entry & 0xFFFFFFFE00000ull) + (virt & 0x1FFFFFull); // 21비트 offset
    }

    // 4KB Page
    current_table = (page_table_t*)ENTRY_TO_VIRT(pd_entry);
    pt_entry_t pt_entry = current_table->entries[indices[3]];
    if (!(pt_entry & X86_PTE_PRESENT)) return 0;

    return (pt_entry & PAGE_ADDR_MASK) + (virt & (PAGE_SIZE - 1));
}

page_table_t* vmm_create_user_map(void) {
    page_t* pml4_page = pmm_alloc_pages(0);
    if (!pml4_page) return NULL;

    page_table_t* user_pml4 = (page_table_t*)p2v(page_to_phys(pml4_page));
    memset(user_pml4, 0, PAGE_SIZE);

    for (int i = 256; i < 512; i++) {
        user_pml4->entries[i] = g_kernel_pagemap->entries[i];
    }

    return user_pml4;
}

void vmm_destroy_map(page_table_t* pml4) {
    if (!pml4 || pml4 == g_kernel_pagemap) return;

    for (int i = 0; i < 256; i++) {
        pt_entry_t pml4e = pml4->entries[i];
        if (!(pml4e & X86_PTE_PRESENT) || (pml4e & X86_PTE_HUGE)) continue;

        page_table_t* pdpt = (page_table_t*)ENTRY_TO_VIRT(pml4e);
        
        for (int j = 0; j < 512; j++) {
            pt_entry_t pdpte = pdpt->entries[j];
            if (!(pdpte & X86_PTE_PRESENT) || (pdpte & X86_PTE_HUGE)) continue;

            page_table_t* pd = (page_table_t*)ENTRY_TO_VIRT(pdpte);
            
            for (int k = 0; k < 512; k++) {
                pt_entry_t pde = pd->entries[k];
                if (!(pde & X86_PTE_PRESENT)) continue;

                if (pde & X86_PTE_HUGE) {
                    uint64_t phys = pde & PAGE_ADDR_MASK;
                    page_t *pg = phys_to_page(phys);
                    if (pg && pg->ref_count > 0) {
                        pg->ref_count--;
                        if (pg->ref_count == 0) {
                            page_free(pg, 9);
                        }
                    }
                    continue;
                }

                page_table_t* pt = (page_table_t*)ENTRY_TO_VIRT(pde);
                for (int l = 0; l < 512; l++) {
                    pt_entry_t pte = pt->entries[l];
                    if (pte & X86_PTE_PRESENT) {
                        uint64_t phys = pte & PAGE_ADDR_MASK;
                        page_t *pg = phys_to_page(phys);
                        if (pg && pg->ref_count > 0) {
                            pg->ref_count--;
                            if (pg->ref_count == 0) {
                                page_free(pg, 0);
                            }
                        }
                    }
                }
                pmm_free_pages(phys_to_page(v2p(pt)), 0);
            }
            pmm_free_pages(phys_to_page(v2p(pd)), 0);
        }
        pmm_free_pages(phys_to_page(v2p(pdpt)), 0);
    }

    pmm_free_pages(phys_to_page(v2p(pml4)), 0);

    if (vmm_get_current_pml4() == pml4) {
        set_cr3(v2p(g_kernel_pagemap));
    }
}

// generic

static uint64_t mmu_to_x86_flags(uint64_t prot) {
    uint64_t flags = X86_PTE_PRESENT;

    if (prot & MMU_FLAGS_WRITE)   flags |= X86_PTE_WRITABLE;
    if (prot & MMU_FLAGS_USER)    flags |= X86_PTE_USER;
    if (prot & MMU_FLAGS_NOCACHE) flags |= (X86_PTE_PCD | X86_PTE_PWT);
    if (prot & MMU_FLAGS_SHARED)  flags |= X86_PTE_SHARED;
    
    if (!(prot & MMU_FLAGS_EXEC)) flags |= X86_PTE_NX;

    return flags;
}

page_table_t* mmu_create_map(void) {
    return vmm_create_user_map();
}

void mmu_destroy_map(page_table_t* map) {
    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    vmm_destroy_map(map);
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
}

page_t* page_alloc(uint8_t order) {
    return pmm_alloc_pages(order);
}

void page_free(page_t* page, uint8_t order) {
    pmm_free_pages(page, order);
}

bool mmu_map(page_table_t* map, uintptr_t vaddr, uintptr_t paddr, uint64_t prot) {
    uint64_t x86_flags = mmu_to_x86_flags(prot);
    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    bool ret;

    if ((vaddr % PAGE_SIZE_2M == 0) && (paddr % PAGE_SIZE_2M == 0)) {
        ret = vmm_map_huge(map, (uint64_t)vaddr, (uint64_t)paddr, x86_flags);
    } else {
        ret = vmm_map(map, (uint64_t)vaddr, (uint64_t)paddr, x86_flags);
    }

    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);

    if (ret) {
        mmu_tlb_shootdown();
    }
    return ret;
}

bool mmu_map_4k(page_table_t* map, uintptr_t vaddr, uintptr_t paddr, uint64_t prot) {
    uint64_t x86_flags = mmu_to_x86_flags(prot);
    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    bool ret = vmm_map(map, (uint64_t)vaddr, (uint64_t)paddr, x86_flags);
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);

    if (ret) {
        mmu_tlb_shootdown();
    }
    return ret;
}

bool mmu_map_demand(page_table_t* map, uintptr_t vaddr, uint64_t prot) {
    uint64_t x86_flags = mmu_to_x86_flags(prot);
    
    x86_flags &= ~X86_PTE_PRESENT;
    x86_flags |= X86_PTE_DEMAND;

    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    pt_entry_t* pte = vmm_get_pte(map, (uint64_t)vaddr, true);
    if (!pte) {
        spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
        return false;
    }

    if (!(*pte & X86_PTE_PRESENT) && !(*pte & X86_PTE_DEMAND)) {
        uintptr_t pt_vaddr = ALIGN_DOWN((uintptr_t)pte, PAGE_SIZE);
        page_t* pt_page = phys_to_page(v2p((void*)pt_vaddr));
        if (pt_page) pt_page->ref_count++;
    }

    *pte = x86_flags;
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);

    mmu_tlb_shootdown();
    return true;
}

void mmu_unmap(page_table_t* map, uintptr_t vaddr) {
    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    vmm_unmap(map, (uint64_t)vaddr);
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);

    mmu_tlb_shootdown();
}

uintptr_t mmu_translate(page_table_t* map, uintptr_t vaddr) {
    if (!map) return 0;
    uint64_t phys = vmm_get_phys(map, (uint64_t)vaddr);
    if (phys == 0) {
        uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
        pt_entry_t *pte = vmm_get_pte(map, (uint64_t)vaddr, false);
        if (pte && (*pte & X86_PTE_DEMAND) && !(*pte & X86_PTE_PRESENT)) {
            page_t *pg = page_alloc(0);
            if (pg) {
                phys = page_to_phys(pg);
                memset(p2v(phys), 0, PAGE_SIZE);

                uint64_t original_flags = *pte;
                original_flags &= ~X86_PTE_DEMAND;
                original_flags |= X86_PTE_PRESENT;

                *pte = phys | original_flags;
                invlpg((uint64_t)vaddr);
            }
        }
        spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
    }
    return (uintptr_t)phys;
}

page_table_t* mmu_get_active_map(void) {
    return vmm_get_current_pml4();
}

page_table_t* mmu_get_kernel_map(void) {
    return g_kernel_pagemap;
}

void mmu_protect_page(page_table_t* map, uintptr_t vaddr, int prot) {
    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    pt_entry_t* pte = vmm_get_pte(map, (uint64_t)vaddr, false);
    
    if (pte && ((*pte & X86_PTE_PRESENT) || (*pte & X86_PTE_DEMAND))) {
        uint64_t new_flags = X86_PTE_USER;
        if (*pte & X86_PTE_PRESENT) {
            new_flags |= X86_PTE_PRESENT;
        } else {
            new_flags |= X86_PTE_DEMAND;
        }
        
        if (prot & 0x2) { // PROT_WRITE
            new_flags |= X86_PTE_WRITABLE;
        }

        if (!(prot & 0x4)) {
            new_flags |= X86_PTE_NX;
        }

        if (*pte & X86_PTE_SHARED) {
            new_flags |= X86_PTE_SHARED;
        }

        uint64_t paddr_mask = 0x000FFFFFFFFFF000ULL;
        uint64_t paddr = *pte & paddr_mask;
        
        *pte = paddr | new_flags;

        invlpg((uint64_t)vaddr);
    }
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
}

page_table_t *mmu_clone_map(page_table_t *parent_map) {
    if (!parent_map) return NULL;

    page_table_t *child_map = mmu_create_map();
    if (!child_map) return NULL;

    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    for (int i = 0; i < 256; i++) {
        pt_entry_t pml4e = parent_map->entries[i];
        if (!(pml4e & X86_PTE_PRESENT)) continue;

        page_table_t *pdpt = (page_table_t *)ENTRY_TO_VIRT(pml4e);
        for (int j = 0; j < 512; j++) {
            pt_entry_t pdpte = pdpt->entries[j];
            if (!(pdpte & X86_PTE_PRESENT)) continue;

            page_table_t *pd = (page_table_t *)ENTRY_TO_VIRT(pdpte);
            for (int k = 0; k < 512; k++) {
                pt_entry_t pde = pd->entries[k];
                if (!(pde & X86_PTE_PRESENT)) continue;

                if (pde & X86_PTE_HUGE) {
                    uint64_t parent_phys = pde & PAGE_ADDR_MASK;
                    
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21);

                    page_t *child_page = page_alloc(9);
                    if (!child_page) {
                        spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
                        mmu_destroy_map(child_map);
                        return NULL;
                    }
                    uint64_t child_phys = page_to_phys(child_page);

                    memcpy(p2v(child_phys), p2v(parent_phys), PAGE_SIZE_2M);

                    uint64_t flags = pde & ~PAGE_ADDR_MASK;

                    vmm_map_huge(child_map, va, child_phys, flags);
                    continue;
                }

                page_table_t *pt = (page_table_t *)ENTRY_TO_VIRT(pde);
                for (int l = 0; l < 512; l++) {
                    pt_entry_t pte = pt->entries[l];
                    if (!(pte & X86_PTE_PRESENT)) {
                        if (pte & X86_PTE_DEMAND) {
                            uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21) | ((uint64_t)l << 12);
                            vmm_map(child_map, va, 0, pte);
                        }
                        continue;
                    }

                    uint64_t parent_phys = pte & PAGE_ADDR_MASK;
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    uint64_t flags = pte & ~PAGE_ADDR_MASK;

                    if (flags & X86_PTE_WRITABLE) {
                        if (!(flags & X86_PTE_SHARED)) {
                            flags &= ~X86_PTE_WRITABLE;
                            flags |= X86_PTE_COW;
                            pt->entries[l] = parent_phys | flags;
                            invlpg((uint64_t)va);
                        }
                    }

                    page_t *pg = phys_to_page(parent_phys);
                    if (pg) {
                        pg->ref_count++;
                    }

                    vmm_map(child_map, va, parent_phys, flags);
                }
            }
        }
    }
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);

    return child_map;
}

static inline uintptr_t read_cr2(void) {
    uintptr_t val;
    asm volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

extern void panic(const char* description, struct trapframe *regs);

void handle_page_fault(struct trapframe *regs, void *data) {
    (void)data;
    uintptr_t fault_addr = read_cr2() & ~0xFFF;
    uint64_t err_code = regs->err_code;

    // Bit 0 (P) = Present, Bit 1 (W/R) = Write
    if ((err_code & 1) == 0) { // Page not present
        page_table_t *map = mmu_get_active_map();
        if (map) {
            uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
            pt_entry_t *pte = vmm_get_pte(map, fault_addr, false);
            if (pte && (*pte & X86_PTE_DEMAND) && !(*pte & X86_PTE_PRESENT)) {
                page_t *pg = page_alloc(0);
                if (!pg) {
                    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
                    panic("Demand Paging: Out of physical memory", regs);
                }
                uint64_t phys_addr = page_to_phys(pg);
                memset(p2v(phys_addr), 0, PAGE_SIZE);

                uint64_t original_flags = *pte;
                original_flags &= ~X86_PTE_DEMAND;
                original_flags |= X86_PTE_PRESENT;

                *pte = phys_addr | original_flags;

                invlpg((uint64_t)fault_addr);
                spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
                return;
            }
            spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
        }
    }

    if ((err_code & 3) == 3) {
        page_table_t *map = mmu_get_active_map();
        if (map) {
            uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
            pt_entry_t *pte = vmm_get_pte(map, fault_addr, false);
            if (pte && (*pte & X86_PTE_PRESENT) && (*pte & X86_PTE_COW)) {
                uint64_t old_phys = *pte & PAGE_ADDR_MASK;
                page_t *pg = phys_to_page(old_phys);
                
                if (pg) {
                    if (pg->ref_count > 1) {
                        page_t *new_pg = page_alloc(0);
                        if (!new_pg) {
                            spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
                            panic("CoW: Out of physical memory", regs);
                        }
                        uint64_t new_phys = page_to_phys(new_pg);
                        
                        spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
                        memcpy(p2v(new_phys), p2v(old_phys), PAGE_SIZE);      
                        lock_flags = spin_lock_irqsave(&g_mmu_lock);
                        
                        pt_entry_t *re_pte = vmm_get_pte(map, fault_addr, false);
                        if (re_pte && re_pte == pte && (*re_pte & X86_PTE_PRESENT) && (*re_pte & X86_PTE_COW) && ((*re_pte & PAGE_ADDR_MASK) == old_phys)) {
                            if (pg->ref_count > 1) {
                                // CoW 삭제 & WRITABLE 복구
                                uint64_t flags = *re_pte & ~PAGE_ADDR_MASK;
                                flags &= ~X86_PTE_COW;
                                flags |= X86_PTE_WRITABLE;
                                *re_pte = new_phys | flags;
                                
                                pg->ref_count--;
                            } else {
                                uint64_t flags = *re_pte & ~PAGE_ADDR_MASK;
                                flags &= ~X86_PTE_COW;
                                flags |= X86_PTE_WRITABLE;
                                *re_pte = old_phys | flags;
                                
                                page_free(new_pg, 0);
                            }
                        } else {
                            page_free(new_pg, 0);
                        }
                    } else {
                        uint64_t flags = *pte & ~PAGE_ADDR_MASK;
                        flags &= ~X86_PTE_COW;
                        flags |= X86_PTE_WRITABLE;
                        *pte = old_phys | flags;
                    }
                    
                    invlpg((uint64_t)fault_addr);
                    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
                    return;
                }
            }
            spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
        }
    }

    // OH SEGFAULT
    if ((regs->cs & 3) == 3) {
        dprintf("[USER PAGE FAULT] PID: %d, Thread: %d, Faulting Addr (CR2): 0x%lx, RIP: 0x%llx, ErrCode: 0x%llx\n",
                curproc ? curproc->p_pid : -1, curthread ? curthread->t_tid : -1,
                read_cr2(), regs->rip, regs->err_code);
        if (curproc && curthread) {
            uint64_t lock_flags = spin_lock_irqsave(&curproc->p_lock);
            curthread->t_sig_pending |= (1ULL << (SIGSEGV - 1));
            spin_unlock_irqrestore(&curproc->p_lock, lock_flags);
            thread_signal_wakeup(curthread);
            return;
        }
    }


    panic("Segmentation Fault", regs);
}

void mmu_flush_cache(void* addr, size_t size) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    for (uintptr_t curr = start & ~63UL; curr < end; curr += 64) {
        asm volatile("clflush (%0)" : : "r"(curr) : "memory");
    }
}

void mmu_tlb_shootdown(void) {
    if (g_intc && g_intc->send_ipi) {
        struct cpu *this_cpu = curcpu;
        for (uint32_t i = 0; i < g_boot_info.smp.total_cores; i++) {
            if (i != this_cpu->id) {
                g_intc->send_ipi(cpus[i].hw_id, 0x46);
            }
        }
    }
}

bool mmu_is_mapped(page_table_t* map, uintptr_t virt) {
    if (!map) return false;
    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    pt_entry_t *pte = vmm_get_pte(map, (uint64_t)virt, false);
    bool ret = false;
    if (pte && (*pte & (X86_PTE_PRESENT | X86_PTE_DEMAND))) {
        ret = true;
    }
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
    return ret;
}

uint64_t mmu_get_flags(page_table_t* map, uintptr_t virt) {
    if (!map) return 0;
    uint64_t lock_flags = spin_lock_irqsave(&g_mmu_lock);
    pt_entry_t *pte = vmm_get_pte(map, (uint64_t)virt, false);
    uint64_t flags = 0;
    if (pte && ((*pte & X86_PTE_PRESENT) || (*pte & X86_PTE_DEMAND))) {
        flags |= MMU_FLAGS_READ;
        if (*pte & X86_PTE_WRITABLE) {
            flags |= MMU_FLAGS_WRITE;
        }
        if (!(*pte & X86_PTE_NX)) {
            flags |= MMU_FLAGS_EXEC;
        }
        if (*pte & X86_PTE_SHARED) {
            flags |= MMU_FLAGS_SHARED;
        }
        if (*pte & X86_PTE_USER) {
            flags |= MMU_FLAGS_USER;
        }
    }
    spin_unlock_irqrestore(&g_mmu_lock, lock_flags);
    return flags;
}

mem_initcall(pmm_init, PRIO_FIRST);
mem_initcall(vmm_init, PRIO_SECOND);