#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/list.h>
#include <kernel/syscall.h>

#include <string.h>

int64_t sys_mmap(uintptr_t addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    (void)flags;

    if (length == 0) return -EINVAL;

    if (fd >= 0 && fd < MAX_FILES) {
        struct file *f = curproc->p_fd_table[fd];
        if (f && f->f_vn && strcmp(f->f_vn->v_name, "fb0") == 0) {
            size_t fb_size = g_boot_info.fb.pitch * g_boot_info.fb.height;
            size_t aligned_fb_len = ALIGN_UP(fb_size, PAGE_SIZE);
            if (length > aligned_fb_len) length = aligned_fb_len;

            if (addr == 0) {
                addr = 0x500000000000;
                while (1) {
                    bool range_free = true;
                    for (uintptr_t off = 0; off < aligned_fb_len; off += PAGE_SIZE) {
                        if (mmu_translate(curproc->p_vm_map, addr + off) != 0) {
                            range_free = false;
                            addr = ALIGN_UP(addr + off + PAGE_SIZE, PAGE_SIZE);
                            break;
                        }
                    }
                    if (range_free) break;
                }
            }

            uintptr_t phys_fb = mmu_translate(mmu_get_kernel_map(), (uintptr_t)g_boot_info.fb.fb_addr);
            if (phys_fb == 0) {
                phys_fb = (uintptr_t)g_boot_info.fb.fb_addr - g_boot_info.hhdm_offset;
            }
            uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);

            for (uintptr_t i = 0; i < aligned_fb_len; i += PAGE_SIZE) {
                uint32_t mmu_flags = MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_READ | MMU_FLAGS_SHARED;
                if (!mmu_map_4k(curproc->p_vm_map, start + i, phys_fb + i, mmu_flags)) {
                    return -ENOMEM;
                }
            }
            return start;
        }
    }

    bool map_fixed = (flags & 0x10); // MAP_FIXED
    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    bool need_search = (addr == 0);

    if (map_fixed) {
        if (addr == 0 || (addr % PAGE_SIZE) != 0) {
            return -EINVAL;
        }
        need_search = false;

        // Forcefully unmap existing mappings in the requested range for MAP_FIXED
        uintptr_t start_unmap = ALIGN_DOWN(addr, PAGE_SIZE);
        uintptr_t end_unmap = ALIGN_UP(addr + length, PAGE_SIZE);
        for (uintptr_t i = start_unmap; i < end_unmap; i += PAGE_SIZE) {
            uintptr_t paddr = mmu_translate(curproc->p_vm_map, i);
            if (paddr != 0) {
                mmu_unmap(curproc->p_vm_map, i);
            }
        }
    } else if (addr != 0) {
        uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);
        uintptr_t end = ALIGN_UP(addr + length, PAGE_SIZE);
        
        if (!is_user_address_range((void*)start, end - start)) {
            need_search = true;
        } else {
            for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
                if (mmu_translate(curproc->p_vm_map, i) != 0) {
                    need_search = true;
                    break;
                }
            }
        }
    }

    if (need_search) {
        uintptr_t search_start = 0x400000000000;
        uintptr_t found_addr = 0;

        while (search_start < curproc->p_mmap_base) {
            bool range_free = true;
            for (uintptr_t offset_val = 0; offset_val < aligned_len; offset_val += PAGE_SIZE) {
                if (mmu_translate(curproc->p_vm_map, search_start + offset_val) != 0) {
                    range_free = false;
                    search_start = ALIGN_UP(search_start + offset_val + PAGE_SIZE, PAGE_SIZE);
                    break;
                }
            }
            if (range_free) {
                found_addr = search_start;
                break;
            }
        }

        if (found_addr != 0) {
            addr = found_addr;
        } else {
            uintptr_t fallback_start = curproc->p_mmap_base;
            while (1) {
                bool range_free = true;
                for (uintptr_t offset_val = 0; offset_val < aligned_len; offset_val += PAGE_SIZE) {
                    if (mmu_translate(curproc->p_vm_map, fallback_start + offset_val) != 0) {
                        range_free = false;
                        fallback_start = ALIGN_UP(fallback_start + offset_val + PAGE_SIZE, PAGE_SIZE);
                        break;
                    }
                }
                if (range_free) {
                    break;
                }
            }
            addr = fallback_start;
            curproc->p_mmap_base = fallback_start + aligned_len;
        }
    }

    uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);
    uintptr_t end = ALIGN_UP(addr + length, PAGE_SIZE);

    struct file *f = NULL;
    if (fd >= 0 && fd < MAX_FILES) {
        f = curproc->p_fd_table[fd];
    }

    for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
        page_t *pg = page_alloc(0);
        if (!pg) return -ENOMEM;

        uintptr_t paddr = page_to_phys(pg);
        memset(p2v(paddr), 0, PAGE_SIZE);

        if (f && f->f_vn && f->f_vn->ops->read) {
            int64_t file_offset = offset + (i - start);
            size_t to_read = 0;
            if (length > (i - start)) {
                to_read = length - (i - start);
                if (to_read > PAGE_SIZE) {
                    to_read = PAGE_SIZE;
                }
            }
            if (to_read > 0) {
                int n = f->f_vn->ops->read(f->f_vn, p2v(paddr), to_read, file_offset);
                if (n < 0) {
                    dprintf("[KERNEL sys_mmap] File read error %d at offset %ld\n", n, file_offset);
                    page_free(pg, 0);
                    return -EIO;
                }
            }
        }

        uint32_t mmu_flags = MMU_FLAGS_USER;
        if (prot & 0x2) mmu_flags |= MMU_FLAGS_WRITE; // PROT_WRITE
        if (flags & 0x1) mmu_flags |= MMU_FLAGS_SHARED; // MAP_SHARED

        if (!mmu_map_4k(curproc->p_vm_map, i, paddr, mmu_flags)) {
            page_free(pg, 0);
            return -ENOMEM;
        }
    }

    return addr;
}

int64_t sys_munmap(void *addr, size_t length) {
    uintptr_t uaddr = (uintptr_t)addr;
    if (length == 0) return -EINVAL;
    if (!is_user_address_range(addr, length)) return -EINVAL;

    uintptr_t start = ALIGN_DOWN(uaddr, PAGE_SIZE);
    uintptr_t end = ALIGN_UP(uaddr + length, PAGE_SIZE);
    page_table_t *map = curproc->p_vm_map;

    for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
        uintptr_t paddr = mmu_translate(map, i);
        if (paddr != 0) {
            mmu_unmap(map, i);
        }
    }

    return 0;
}

int64_t sys_brk(uintptr_t brk) {
    uintptr_t old_brk = curproc->p_brk;
    if (brk == 0) {
        return old_brk;
    }

    if (brk > old_brk) {
        uintptr_t start = ALIGN_UP(old_brk, PAGE_SIZE);
        uintptr_t end = ALIGN_UP(brk, PAGE_SIZE);

        if (end > curproc->p_stack_top) {
            return old_brk;
        }

        for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
            if (mmu_translate(curproc->p_vm_map, i) == 0) {
                page_t* pg = page_alloc(0);
                if (!pg) {
                    return old_brk;
                }
                uintptr_t paddr = page_to_phys(pg);
                memset(p2v(paddr), 0, PAGE_SIZE);
                if (!mmu_map_4k(curproc->p_vm_map, i, paddr, MMU_FLAGS_USER | MMU_FLAGS_WRITE)) {
                    page_free(pg, 0);
                    return old_brk; 
                }
            }
        }
    } else if (brk < old_brk) {
        uintptr_t start = ALIGN_UP(brk, PAGE_SIZE);
        uintptr_t end = ALIGN_UP(old_brk, PAGE_SIZE);
        page_table_t *map = curproc->p_vm_map;

        for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
            uintptr_t paddr = mmu_translate(map, i);
            if (paddr != 0) {
                mmu_unmap(map, i);
            }
        }
    }

    curproc->p_brk = brk;
    return brk;
}

int64_t sys_mprotect(uintptr_t start, size_t len, int prot) {
    if (len == 0) return 0;
    if (!is_user_address_range((void *)start, len)) return -EINVAL;

    uintptr_t addr = ALIGN_DOWN(start, PAGE_SIZE);
    uintptr_t end = ALIGN_UP(start + len, PAGE_SIZE);
    page_table_t *map = curproc->p_vm_map;

    for (uintptr_t i = addr; i < end; i += PAGE_SIZE) {
        if (mmu_translate(map, i) == 0) {
            return -ENOMEM;
        }
    }

    for (uintptr_t i = addr; i < end; i += PAGE_SIZE) {
        mmu_protect_page(map, i, prot);
    }

    return 0;
}
