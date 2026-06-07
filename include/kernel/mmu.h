#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <boot/bootinfo.h>

#define PAGE_SIZE       4096
#define PAGE_SIZE_2M    (2ULL * 1024 * 1024)
#define MAX_ORDER       11

#define MMU_FLAGS_READ     (1ULL << 0)
#define MMU_FLAGS_WRITE    (1ULL << 1)
#define MMU_FLAGS_EXEC     (1ULL << 2)
#define MMU_FLAGS_USER     (1ULL << 3)
#define MMU_FLAGS_NOCACHE  (1ULL << 4)
#define MMU_FLAGS_SHARED   (1ULL << 5)

#define ALIGN_DOWN(addr, align) ((uintptr_t)(addr) & ~((uintptr_t)(align) - 1))
#define ALIGN_UP(addr, align)   (((uintptr_t)(addr) + (uintptr_t)(align) - 1) & ~((uintptr_t)(align) - 1))

static inline void* p2v(uintptr_t phys) {
    return (void*)(phys + g_boot_info.hhdm_offset);
}

static inline uintptr_t v2p(void* virt) {
    return (uintptr_t)virt - g_boot_info.hhdm_offset;
}

struct page {
    struct page* next;
    struct page* prev;
    
    uint32_t     ref_count;
    uint8_t      order;
    bool         is_free;
    uint16_t     flags;

    uint32_t     obj_size;
    uint32_t     free_count;
    
    void* free_ptr;
    uintptr_t    vaddr;
};

typedef struct page_table page_table_t;
typedef struct page page_t;

page_t* page_alloc(uint8_t order);
void page_free(page_t* page, uint8_t order);

bool mmu_map(page_table_t* map, uintptr_t virt, uintptr_t phys, uint64_t flags);
bool mmu_map_4k(page_table_t* map, uintptr_t virt, uintptr_t phys, uint64_t flags);
bool mmu_map_demand(page_table_t* map, uintptr_t virt, uint64_t flags);
void mmu_unmap(page_table_t* map, uintptr_t virt);
void mmu_protect_page(page_table_t* map, uintptr_t vaddr, int prot);
uintptr_t mmu_translate(page_table_t* map, uintptr_t virt);
bool mmu_is_mapped(page_table_t* map, uintptr_t virt);
uint64_t mmu_get_flags(page_table_t* map, uintptr_t virt);
page_table_t *mmu_clone_map(page_table_t *parent_map);

uint64_t page_to_phys(page_t* page);
page_t* phys_to_page(uint64_t phys);

page_table_t* mmu_get_active_map(void);
page_table_t* mmu_get_kernel_map(void);

page_table_t* mmu_create_map(void);
void mmu_destroy_map(page_table_t* map);

void mmu_flush_cache(void* addr, size_t size);

void mmu_tlb_shootdown(void);