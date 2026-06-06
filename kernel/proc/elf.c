#include <uapi/elf.h>
#include <kernel/kmem.h>
#include <kernel/mmu.h>
#include <kernel/exec.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>
#include <string.h>
#include <kernel/printf.h>

extern struct vnode *g_root_vnode;

page_table_t* load_elf(void *elf_data, uintptr_t *out_entry, uintptr_t *out_brk, 
                      uintptr_t *out_phdr_vaddr, uint64_t *out_phnum,
                      uintptr_t *out_interpreter_base) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return NULL;
    }
    
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uintptr_t)elf_data + ehdr->e_phoff);
    Elf64_Phdr *interp_phdr = NULL;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_INTERP) {
            interp_phdr = &phdrs[i];
            break;
        }
    }

    if (!interp_phdr) {
        dprintf("[KERNEL load_elf] Static binaries (no interpreter) are not supported. Rejecting.\n");
        return NULL;
    }

    char interp_path[256];
    size_t path_len = interp_phdr->p_filesz;
    if (path_len > 255) path_len = 255;
    memcpy(interp_path, (char *)elf_data + interp_phdr->p_offset, path_len);
    interp_path[path_len] = '\0';

    struct vnode *vn_interp = NULL;
    struct vnode *base_vn = (curproc && curproc->p_cwd) ? curproc->p_cwd : g_root_vnode;
    int err = vfs_lookup(interp_path, base_vn, &vn_interp);
    if (err < 0) {
        dprintf("[KERNEL load_elf] Failed to find interpreter %s: %d\n", interp_path, err);
        return NULL;
    }

    size_t allocated_size = 64 * 1024;
    void *interp_data = kmalloc(allocated_size);
    if (!interp_data) {
        vput(vn_interp);
        return NULL;
    }
    size_t file_size = 0;

    while (1) {
        if (file_size >= allocated_size) {
            size_t new_size = allocated_size * 2;
            void *new_buf = kmalloc(new_size);
            if (!new_buf) {
                kfree(interp_data);
                vput(vn_interp);
                return NULL;
            }
            memcpy(new_buf, interp_data, file_size);
            kfree(interp_data);
            interp_data = new_buf;
            allocated_size = new_size;
        }
        size_t space_left = allocated_size - file_size;
        int n = vn_interp->ops->read(vn_interp, (void *)((uintptr_t)interp_data + file_size), space_left, file_size);
        if (n < 0) {
            kfree(interp_data);
            vput(vn_interp);
            return NULL;
        }
        if (n == 0) break;
        file_size += n;
    }
    vput(vn_interp);

    Elf64_Ehdr *iehdr = (Elf64_Ehdr *)interp_data;
    if (memcmp(iehdr->e_ident, ELFMAG, SELFMAG) != 0 || iehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        dprintf("[KERNEL load_elf] Interpreter ELF verification failed\n");
        kfree(interp_data);
        return NULL;
    }

    page_table_t *new_map = mmu_create_map();
    if (!new_map) {
        kfree(interp_data);
        return NULL;
    }

    uintptr_t main_binary_base = (ehdr->e_type == ET_DYN) ? 0x00400000 : 0;
    uintptr_t highest_addr = main_binary_base;
    uintptr_t phdr_vaddr = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_PHDR) {
            phdr_vaddr = main_binary_base + phdrs[i].p_vaddr;
            break;
        }
    }
    if (phdr_vaddr == 0) {
        phdr_vaddr = main_binary_base + 0x200000 + ehdr->e_phoff; 
    }

    // Map main binary's PT_LOAD segments
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uintptr_t vaddr = main_binary_base + phdrs[i].p_vaddr;
        size_t mem_size = phdrs[i].p_memsz;

        uintptr_t start_vaddr = ALIGN_DOWN(vaddr, PAGE_SIZE);
        uintptr_t end_vaddr = ALIGN_UP(vaddr + mem_size, PAGE_SIZE);

        if (vaddr + mem_size > highest_addr) {
            highest_addr = vaddr + mem_size;
        }

        for (uintptr_t curr = start_vaddr; curr < end_vaddr; curr += PAGE_SIZE) {
            if (mmu_translate(new_map, curr) != 0) {
                continue;
            }

            page_t *p = page_alloc(0);
            if (!p) {
                mmu_destroy_map(new_map);
                kfree(interp_data);
                return NULL;
            }
            uintptr_t paddr = page_to_phys(p);
            memset(p2v(paddr), 0, PAGE_SIZE);
            
            uint64_t temp_prot = MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_EXEC;
            mmu_map(new_map, curr, paddr, temp_prot);
        }
    }

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uintptr_t vaddr = main_binary_base + phdrs[i].p_vaddr;
        size_t file_size = phdrs[i].p_filesz;
        uintptr_t file_offset = phdrs[i].p_offset;

        size_t written = 0;
        while (written < file_size) {
            uintptr_t curr_v = vaddr + written;
            uintptr_t phys = mmu_translate(new_map, curr_v);
            
            if (phys == 0) {
                mmu_destroy_map(new_map);
                kfree(interp_data);
                return NULL;
            }

            size_t off_in_page = curr_v % PAGE_SIZE;
            size_t to_copy = MIN(file_size - written, PAGE_SIZE - off_in_page);

            void *dest_addr = (void *)(p2v(phys & ~(PAGE_SIZE - 1)) + off_in_page);
            void *src_addr = (void *)((uintptr_t)elf_data + file_offset + written);

            memcpy(dest_addr, src_addr, to_copy);
            mmu_flush_cache(dest_addr, to_copy);
            written += to_copy;
        }
    }

    uintptr_t interp_base = 0x0d001000;
    Elf64_Phdr *iphdrs = (Elf64_Phdr *)((uintptr_t)interp_data + iehdr->e_phoff);

    for (int j = 0; j < iehdr->e_phnum; j++) {
        if (iphdrs[j].p_type != PT_LOAD) continue;

        uintptr_t vaddr = interp_base + iphdrs[j].p_vaddr;
        size_t mem_size = iphdrs[j].p_memsz;

        uintptr_t start_vaddr = ALIGN_DOWN(vaddr, PAGE_SIZE);
        uintptr_t end_vaddr = ALIGN_UP(vaddr + mem_size, PAGE_SIZE);

        for (uintptr_t curr = start_vaddr; curr < end_vaddr; curr += PAGE_SIZE) {
            if (mmu_translate(new_map, curr) != 0) {
                continue;
            }

            page_t *p = page_alloc(0);
            if (!p) {
                mmu_destroy_map(new_map);
                kfree(interp_data);
                return NULL;
            }
            uintptr_t paddr = page_to_phys(p);
            memset(p2v(paddr), 0, PAGE_SIZE);

            uint64_t temp_prot = MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_EXEC;
            mmu_map_4k(new_map, curr, paddr, temp_prot);
        }
    }

    for (int j = 0; j < iehdr->e_phnum; j++) {
        if (iphdrs[j].p_type != PT_LOAD) continue;

        uintptr_t vaddr = interp_base + iphdrs[j].p_vaddr;
        size_t file_size = iphdrs[j].p_filesz;
        uintptr_t file_offset = iphdrs[j].p_offset;

        size_t written = 0;
        while (written < file_size) {
            uintptr_t curr_v = vaddr + written;
            uintptr_t phys = mmu_translate(new_map, curr_v);

            if (phys == 0) {
                mmu_destroy_map(new_map);
                kfree(interp_data);
                return NULL;
            }

            size_t off_in_page = curr_v % PAGE_SIZE;
            size_t to_copy = MIN(file_size - written, PAGE_SIZE - off_in_page);

            void *dest_addr = (void *)(p2v(phys & ~(PAGE_SIZE - 1)) + off_in_page);
            void *src_addr = (void *)((uintptr_t)interp_data + file_offset + written);

            memcpy(dest_addr, src_addr, to_copy);
            mmu_flush_cache(dest_addr, to_copy);
            written += to_copy;
        }
    }

    uintptr_t interp_entry = interp_base + iehdr->e_entry;

    kfree(interp_data);

    *out_entry = interp_entry;
    *out_interpreter_base = interp_base;
    *out_brk = highest_addr;
    *out_phdr_vaddr = phdr_vaddr;
    *out_phnum = ehdr->e_phnum;
    
    return new_map;}