#include <uapi/elf.h>

#include <kernel/kmem.h>
#include <kernel/mmu.h>
#include <kernel/exec.h>

#include <string.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

page_table_t* load_elf(void *elf_data, uintptr_t *out_entry, uintptr_t *out_brk) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return NULL;
    }

    page_table_t *new_map = mmu_create_map();
    if (!new_map) return NULL;

    uintptr_t highest_addr = 0;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uintptr_t)elf_data + ehdr->e_phoff);
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uintptr_t vaddr = phdrs[i].p_vaddr;
        size_t mem_size = phdrs[i].p_memsz;
        size_t file_size = phdrs[i].p_filesz;
        uintptr_t file_offset = phdrs[i].p_offset;

        uintptr_t start_vaddr = ALIGN_DOWN(vaddr, PAGE_SIZE);
        uintptr_t end_vaddr = ALIGN_UP(vaddr + mem_size, PAGE_SIZE);

        if (vaddr + mem_size > highest_addr) {
            highest_addr = vaddr + mem_size;
        }
        
        uint64_t prot = MMU_FLAGS_USER;
        if (phdrs[i].p_flags & PF_W) prot |= MMU_FLAGS_WRITE;
        if (phdrs[i].p_flags & PF_X) prot |= MMU_FLAGS_EXEC;

        for (uintptr_t curr = start_vaddr; curr < end_vaddr; curr += PAGE_SIZE) {
            if (mmu_translate(new_map, curr) != 0) continue;

            page_t *p = page_alloc(0);
            if (!p) {
                mmu_destroy_map(new_map);
                return NULL;
            }
            uintptr_t paddr = page_to_phys(p);
            
            uint64_t temp_prot = prot | MMU_FLAGS_WRITE;
            memset(p2v(paddr), 0, PAGE_SIZE);
            mmu_map(new_map, curr, paddr, temp_prot);
        }

        // asm volatile ("stac");
        for (size_t written = 0; written < file_size; ) {
            uintptr_t curr_v = vaddr + written;
            uintptr_t phys = mmu_translate(new_map, curr_v);

            if (phys == 0) {
                // asm volatile ("clac");
                mmu_destroy_map(new_map);
                return NULL;
            }
            
            size_t off_in_page = curr_v % PAGE_SIZE;
            size_t to_copy = MIN(file_size - written, PAGE_SIZE - off_in_page);
            
            memcpy((void *)(p2v(phys) + off_in_page), 
                   (void *)((uintptr_t)elf_data + file_offset + written), 
                   to_copy);
                   
            written += to_copy;
        }
        // asm volatile ("clac");
    }

    *out_entry = ehdr->e_entry;
    *out_brk = highest_addr;
    return new_map;
}