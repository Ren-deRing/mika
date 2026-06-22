#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <kernel/fs/vnode.h>
#include <kernel/fs/file.h>
#include <kernel/lock.h>

#include <string.h>

struct shared_page_entry {
    struct vnode *vn;
    int64_t file_offset;
    uintptr_t phys_addr;
};

#define MAX_SHARED_PAGES 16384
static struct shared_page_entry g_shared_pages[MAX_SHARED_PAGES];
static spinlock_t g_shared_pages_lock = SPINLOCK_INITIALIZER;

static void cleanup_stale_shared_pages_unlocked(void) {
    struct vnode *to_put[32];
    int count = 0;

    uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
        if (g_shared_pages[s].vn != NULL) {
            uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
            page_t *pg = phys_to_page(cached_paddr);
            if (pg && pg->is_free) {
                struct vnode *vn = g_shared_pages[s].vn;
                g_shared_pages[s].vn = NULL;
                g_shared_pages[s].file_offset = 0;
                g_shared_pages[s].phys_addr = 0;
                
                to_put[count++] = vn;
                if (count == 32) {
                    break;
                }
            }
        }
    }
    spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);

    for (int i = 0; i < count; i++) {
        vput(to_put[i]);
    }
}

int64_t sys_mmap(uintptr_t addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    (void)flags;

    if (length == 0) return -EINVAL;

    if (fd >= 0 && fd < MAX_FILES) {
        struct file *f = curproc->p_fd_table[fd];
        if (f && f->f_vn && (strcmp(f->f_vn->v_name, "fb0") == 0 || strcmp(f->f_vn->v_name, "card0") == 0)) {
            size_t fb_size = g_boot_info.fb.pitch * g_boot_info.fb.height;
            size_t aligned_fb_len = ALIGN_UP(fb_size, PAGE_SIZE);

            if (length > aligned_fb_len) length = aligned_fb_len;

            uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);
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
                start = addr;
            }

            uintptr_t phys_fb = mmu_translate(mmu_get_kernel_map(), (uintptr_t)g_boot_info.fb.fb_addr);
            if (phys_fb == 0) {
                phys_fb = (uintptr_t)g_boot_info.fb.fb_addr - g_boot_info.hhdm_offset;
            }

            for (uintptr_t i = 0; i < aligned_fb_len; i += PAGE_SIZE) {
                uint32_t mmu_flags = MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_READ | MMU_FLAGS_SHARED | MMU_FLAGS_NOCACHE;
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
    struct vnode *mapped_vn = NULL;

    if (fd >= 0 && fd < MAX_FILES) {
        uint64_t proc_flags = spin_lock_irqsave(&curproc->p_lock);
        f = curproc->p_fd_table[fd];
        if (f && f->f_vn) {
            mapped_vn = f->f_vn;
            vref(mapped_vn);
        }
        spin_unlock_irqrestore(&curproc->p_lock, proc_flags);
    }

    for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
        uint32_t mmu_flags = MMU_FLAGS_USER;
        if (prot & 0x1) mmu_flags |= MMU_FLAGS_READ; // PROT_READ
        if (prot & 0x2) mmu_flags |= MMU_FLAGS_WRITE; // PROT_WRITE
        if (prot & 0x4) mmu_flags |= MMU_FLAGS_EXEC; // PROT_EXEC
        if (flags & 0x1) mmu_flags |= MMU_FLAGS_SHARED; // MAP_SHARED

        if (mapped_vn == NULL) {
            if (!mmu_map_demand(curproc->p_vm_map, i, mmu_flags)) {
                if (mapped_vn) vput(mapped_vn);
                return -ENOMEM;
            }
        } else {
            int64_t file_offset = offset + (i - start);
            uintptr_t paddr = 0;
            bool newly_allocated = false;
            bool use_shared_cache = (flags & 0x1); // MAP_SHARED

            if (use_shared_cache) {
                struct vnode *to_put = NULL;
                uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                    if (g_shared_pages[s].vn == mapped_vn && g_shared_pages[s].file_offset == file_offset) {
                        uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
                        page_t *pg = phys_to_page(cached_paddr);
                        if (pg && pg->is_free) {
                            to_put = g_shared_pages[s].vn;
                            g_shared_pages[s].vn = NULL;
                            g_shared_pages[s].file_offset = 0;
                            g_shared_pages[s].phys_addr = 0;
                        } else {
                            paddr = cached_paddr;
                            break;
                        }
                    }
                }
                spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);

                if (to_put) {
                    vput(to_put);
                }
            }

            if (paddr == 0) {
                page_t *pg = page_alloc(0);
                if (!pg) {
                    return -ENOMEM;
                }
                uintptr_t allocated_paddr = page_to_phys(pg);
                memset(p2v(allocated_paddr), 0, PAGE_SIZE);

                if (!mapped_vn->ops || !mapped_vn->ops->read) {
                    page_free(pg, 0);
                    if (mapped_vn) vput(mapped_vn);
                    return -EBADF;
                }

                int n = mapped_vn->ops->read(mapped_vn, p2v(allocated_paddr), PAGE_SIZE, file_offset);
                if (n < 0) {
                    dprintf("[KERNEL sys_mmap] File read error %d at offset %ld\n", n, file_offset);
                    page_free(pg, 0);
                    return -EIO;
                }

                if (use_shared_cache) {
                    cleanup_stale_shared_pages_unlocked();

                    uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                    uintptr_t double_check_paddr = 0;
                    struct vnode *to_put_dc = NULL;
                    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                        if (g_shared_pages[s].vn == mapped_vn && g_shared_pages[s].file_offset == file_offset) {
                            uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
                            page_t *cached_pg = phys_to_page(cached_paddr);
                            if (cached_pg && cached_pg->is_free) {
                                to_put_dc = mapped_vn;
                                g_shared_pages[s].vn = NULL;
                                g_shared_pages[s].file_offset = 0;
                                g_shared_pages[s].phys_addr = 0;
                            } else {
                                double_check_paddr = cached_paddr;
                                break;
                            }
                        }
                    }

                    if (double_check_paddr != 0) {
                        spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                        if (to_put_dc) {
                            vput(to_put_dc);
                        }
                        page_free(pg, 0);
                        paddr = double_check_paddr;
                    } else {
                        bool registered = false;
                        for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                            if (g_shared_pages[s].vn == NULL) {
                                g_shared_pages[s].vn = mapped_vn;
                                vref(mapped_vn);
                                g_shared_pages[s].file_offset = file_offset;
                                g_shared_pages[s].phys_addr = allocated_paddr;
                                registered = true;
                                break;
                            }
                        }
                        spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);

                        if (to_put_dc) {
                            vput(to_put_dc);
                        }

                        if (!registered) {
                            dprintf("[KERNEL sys_mmap] WARNING: Shared pages cache is full!\n");
                        }
                        paddr = allocated_paddr;
                        newly_allocated = true;
                    }
                } else {
                    paddr = allocated_paddr;
                    newly_allocated = true;
                }
            }

            if (!mmu_map_4k(curproc->p_vm_map, i, paddr, mmu_flags)) {
                if (newly_allocated) {
                    if (use_shared_cache) {
                        struct vnode *to_put_err = NULL;
                        uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                        for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                        if (g_shared_pages[s].vn == mapped_vn && g_shared_pages[s].file_offset == file_offset) {
                                to_put_err = g_shared_pages[s].vn;
                                g_shared_pages[s].vn = NULL;
                                g_shared_pages[s].file_offset = 0;
                                g_shared_pages[s].phys_addr = 0;
                                break;
                            }
                        }
                        spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                        
                        if (to_put_err) {
                            vput(to_put_err);
                        }
                    }
                    page_free(phys_to_page(paddr), 0);
                }
                return -ENOMEM;
            }
        }
    }

    if (mapped_vn) {
        vput(mapped_vn);
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
                if (!mmu_map_demand(curproc->p_vm_map, i, MMU_FLAGS_USER | MMU_FLAGS_WRITE)) {
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

int64_t sys_mremap(uintptr_t old_addr, size_t old_size, size_t new_size, int flags, uintptr_t new_addr) {
    if (old_addr == 0 || (old_addr % PAGE_SIZE) != 0) {
        return -EINVAL;
    }

    size_t aligned_old = ALIGN_UP(old_size, PAGE_SIZE);
    size_t aligned_new = ALIGN_UP(new_size, PAGE_SIZE);

    if (aligned_old == aligned_new) {
        return old_addr;
    }

    if (aligned_new < aligned_old) {
        uintptr_t unmap_start = old_addr + aligned_new;
        for (uintptr_t i = unmap_start; i < old_addr + aligned_old; i += PAGE_SIZE) {
            uintptr_t paddr = mmu_translate(curproc->p_vm_map, i);
            if (paddr != 0) {
                mmu_unmap(curproc->p_vm_map, i);
            }
        }
        return old_addr;
    }

    bool may_move = (flags & 1); // MREMAP_MAYMOVE
    bool map_fixed = (flags & 2); // MREMAP_FIXED

    if (map_fixed) {
        return -EINVAL;
    }

    uintptr_t old_paddr = mmu_translate(curproc->p_vm_map, old_addr);
    uint32_t orig_mmu_flags = mmu_get_flags(curproc->p_vm_map, old_addr);
    if (orig_mmu_flags == 0) {
        orig_mmu_flags = MMU_FLAGS_USER | MMU_FLAGS_READ;
    }

    struct vnode *orig_vn = NULL;
    int64_t orig_offset = 0;
    bool is_shared_file = false;

    if (old_paddr != 0) {
        uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
        for (int s = 0; s < MAX_SHARED_PAGES; s++) {
            if (g_shared_pages[s].phys_addr == old_paddr && g_shared_pages[s].vn != NULL) {
                orig_vn = g_shared_pages[s].vn;
                orig_offset = g_shared_pages[s].file_offset;
                is_shared_file = true;
                break;
            }
        }
        spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
    }

    bool can_extend_inplace = true;
    for (uintptr_t i = old_addr + aligned_old; i < old_addr + aligned_new; i += PAGE_SIZE) {
        if (mmu_is_mapped(curproc->p_vm_map, i)) {
            can_extend_inplace = false;
            break;
        }
    }

    if (can_extend_inplace) {
        for (uintptr_t i = old_addr + aligned_old; i < old_addr + aligned_new; i += PAGE_SIZE) {
            if (is_shared_file) {
                int64_t file_offset = orig_offset + (i - old_addr);
                uintptr_t paddr = 0;
                bool newly_allocated = false;

                uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                    if (g_shared_pages[s].vn == orig_vn && g_shared_pages[s].file_offset == file_offset) {
                        uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
                        page_t *pg = phys_to_page(cached_paddr);
                        if (pg && pg->is_free) {
                            g_shared_pages[s].vn = NULL;
                            g_shared_pages[s].file_offset = 0;
                            g_shared_pages[s].phys_addr = 0;
                        } else {
                            paddr = cached_paddr;
                            break;
                        }
                    }
                }
                spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);

                if (paddr == 0) {
                    page_t *pg = page_alloc(0);
                    if (!pg) {
                        return -ENOMEM;
                    }
                    uintptr_t allocated_paddr = page_to_phys(pg);
                    memset(p2v(allocated_paddr), 0, PAGE_SIZE);

                    int n = orig_vn->ops->read(orig_vn, p2v(allocated_paddr), PAGE_SIZE, file_offset);
                    if (n < 0) {
                        dprintf("[sys_mremap] File read error %d at offset %ld during inplace grow\n", n, file_offset);
                        page_free(pg, 0);
                        return -EIO;
                    }

                    cleanup_stale_shared_pages_unlocked();
                    uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                    uintptr_t double_check_paddr = 0;
                    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                        if (g_shared_pages[s].vn == orig_vn && g_shared_pages[s].file_offset == file_offset) {
                            uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
                            page_t *cached_pg = phys_to_page(cached_paddr);
                            if (cached_pg && cached_pg->is_free) {
                                g_shared_pages[s].vn = NULL;
                                g_shared_pages[s].file_offset = 0;
                                g_shared_pages[s].phys_addr = 0;
                            } else {
                                double_check_paddr = cached_paddr;
                                break;
                            }
                        }
                    }

                    if (double_check_paddr != 0) {
                        spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                        page_free(pg, 0);
                        paddr = double_check_paddr;
                    } else {
                        bool registered = false;
                        for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                            if (g_shared_pages[s].vn == NULL) {
                                g_shared_pages[s].vn = orig_vn;
                                vref(orig_vn);
                                g_shared_pages[s].file_offset = file_offset;
                                g_shared_pages[s].phys_addr = allocated_paddr;
                                registered = true;
                                break;
                            }
                        }
                        spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                        if (!registered) {
                            dprintf("[sys_mremap] WARNING: Shared pages cache is full during inplace grow!\n");
                        }
                        paddr = allocated_paddr;
                        newly_allocated = true;
                    }
                }

                if (!mmu_map_4k(curproc->p_vm_map, i, paddr, orig_mmu_flags)) {
                    if (newly_allocated) {
                        uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                        for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                            if (g_shared_pages[s].vn == orig_vn && g_shared_pages[s].file_offset == file_offset) {
                                g_shared_pages[s].vn = NULL;
                                g_shared_pages[s].file_offset = 0;
                                g_shared_pages[s].phys_addr = 0;
                                break;
                            }
                        }
                        spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                        page_free(phys_to_page(paddr), 0);
                    }
                    return -ENOMEM;
                }
            } else {
                if (!mmu_map_demand(curproc->p_vm_map, i, orig_mmu_flags)) {
                    return -ENOMEM;
                }
            }
        }
        return old_addr;
    }

    if (!may_move) {
        return -ENOMEM;
    }

    int64_t new_vaddr_or_err = sys_mmap(0, aligned_new, 3, 34, -1, 0); // PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS
    if (new_vaddr_or_err < 0) {
        return new_vaddr_or_err;
    }
    uintptr_t new_vaddr = (uintptr_t)new_vaddr_or_err;

    for (uintptr_t offset = 0; offset < aligned_old; offset += PAGE_SIZE) {
        uintptr_t old_page_vaddr = old_addr + offset;
        uintptr_t new_page_vaddr = new_vaddr + offset;

        mmu_unmap(curproc->p_vm_map, new_page_vaddr);

        uintptr_t paddr = mmu_translate(curproc->p_vm_map, old_page_vaddr);
        if (paddr != 0) {
            mmu_map_4k(curproc->p_vm_map, new_page_vaddr, paddr, orig_mmu_flags);
            mmu_unmap(curproc->p_vm_map, old_page_vaddr);
        } else {
            if (is_shared_file) {
                int64_t file_offset = orig_offset + offset;
                uintptr_t cached_paddr = 0;

                uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                    if (g_shared_pages[s].vn == orig_vn && g_shared_pages[s].file_offset == file_offset) {
                        cached_paddr = g_shared_pages[s].phys_addr;
                        break;
                    }
                }
                spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);

                if (cached_paddr != 0) {
                    mmu_map_4k(curproc->p_vm_map, new_page_vaddr, cached_paddr, orig_mmu_flags);
                } else {
                    mmu_map_demand(curproc->p_vm_map, new_page_vaddr, orig_mmu_flags);
                }
                mmu_unmap(curproc->p_vm_map, old_page_vaddr);
            } else {
                mmu_map_demand(curproc->p_vm_map, new_page_vaddr, orig_mmu_flags);
                mmu_unmap(curproc->p_vm_map, old_page_vaddr);
            }
        }
    }

    for (uintptr_t offset = aligned_old; offset < aligned_new; offset += PAGE_SIZE) {
        uintptr_t new_page_vaddr = new_vaddr + offset;

        mmu_unmap(curproc->p_vm_map, new_page_vaddr);

        if (is_shared_file) {
            int64_t file_offset = orig_offset + offset;
            uintptr_t paddr = 0;
            bool newly_allocated = false;

            uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
            for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                if (g_shared_pages[s].vn == orig_vn && g_shared_pages[s].file_offset == file_offset) {
                    uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
                    page_t *pg = phys_to_page(cached_paddr);
                    if (pg && pg->is_free) {
                        g_shared_pages[s].vn = NULL;
                        g_shared_pages[s].file_offset = 0;
                        g_shared_pages[s].phys_addr = 0;
                    } else {
                        paddr = cached_paddr;
                        break;
                    }
                }
            }
            spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);

            if (paddr == 0) {
                page_t *pg = page_alloc(0);
                if (!pg) {
                    return -ENOMEM;
                }
                uintptr_t allocated_paddr = page_to_phys(pg);
                memset(p2v(allocated_paddr), 0, PAGE_SIZE);

                int n = orig_vn->ops->read(orig_vn, p2v(allocated_paddr), PAGE_SIZE, file_offset);
                if (n < 0) {
                    dprintf("[sys_mremap] File read error %d at offset %ld during moved grow\n", n, file_offset);
                    page_free(pg, 0);
                    return -EIO;
                }

                cleanup_stale_shared_pages_unlocked();
                uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                uintptr_t double_check_paddr = 0;
                for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                    if (g_shared_pages[s].vn == orig_vn && g_shared_pages[s].file_offset == file_offset) {
                        uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
                        page_t *cached_pg = phys_to_page(cached_paddr);
                        if (cached_pg && cached_pg->is_free) {
                            g_shared_pages[s].vn = NULL;
                            g_shared_pages[s].file_offset = 0;
                            g_shared_pages[s].phys_addr = 0;
                        } else {
                            double_check_paddr = cached_paddr;
                            break;
                        }
                    }
                }

                if (double_check_paddr != 0) {
                    spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                    page_free(pg, 0);
                    paddr = double_check_paddr;
                } else {
                    bool registered = false;
                    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                        if (g_shared_pages[s].vn == NULL) {
                            g_shared_pages[s].vn = orig_vn;
                            vref(orig_vn);
                            g_shared_pages[s].file_offset = file_offset;
                            g_shared_pages[s].phys_addr = allocated_paddr;
                            registered = true;
                            break;
                        }
                    }
                    spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                    if (!registered) {
                        dprintf("[sys_mremap] WARNING: Shared pages cache is full during moved grow!\n");
                    }
                    paddr = allocated_paddr;
                    newly_allocated = true;
                }
            }

            if (!mmu_map_4k(curproc->p_vm_map, new_page_vaddr, paddr, orig_mmu_flags)) {
                if (newly_allocated) {
                    uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
                        if (g_shared_pages[s].vn == orig_vn && g_shared_pages[s].file_offset == file_offset) {
                            g_shared_pages[s].vn = NULL;
                            g_shared_pages[s].file_offset = 0;
                            g_shared_pages[s].phys_addr = 0;
                            break;
                        }
                    }
                    spin_unlock_irqrestore(&g_shared_pages_lock, lock_flags);
                    page_free(phys_to_page(paddr), 0);
                }
                return -ENOMEM;
            }
        } else {
            if (!mmu_map_demand(curproc->p_vm_map, new_page_vaddr, orig_mmu_flags)) {
                return -ENOMEM;
            }
        }
    }

    return new_vaddr;
}
