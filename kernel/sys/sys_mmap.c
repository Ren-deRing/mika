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
#include <kernel/vma.h>

#include <string.h>

// Shared-page cache
struct shared_page_entry {
    struct vnode *vn;
    int64_t file_offset;
    uintptr_t phys_addr;
};

#define MAX_SHARED_PAGES 16384
static struct shared_page_entry g_shared_pages[MAX_SHARED_PAGES];
static spinlock_t g_shared_pages_lock = SPINLOCK_INITIALIZER;

static void cleanup_stale_shared_pages_locked(void) {
    struct vnode *to_put[32];
    int count = 0;

    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
        if (g_shared_pages[s].vn != NULL) {
            uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
            page_t *pg = phys_to_page(cached_paddr);
            if (pg && pg->is_free) {
                struct vnode *vn = g_shared_pages[s].vn;
                g_shared_pages[s].vn = NULL;
                g_shared_pages[s].file_offset = 0;
                g_shared_pages[s].phys_addr = 0;
                if (pg->ref_count > 0) pg->ref_count--;

                to_put[count++] = vn;
                if (count == 32) break;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        vput(to_put[i]);
    }
}

uintptr_t vma_shared_lookup(struct vnode *vn, int64_t offset) {
    uint64_t flags = spin_lock_irqsave(&g_shared_pages_lock);
    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
        if (g_shared_pages[s].vn == vn && g_shared_pages[s].file_offset == offset) {
            uintptr_t cached_paddr = g_shared_pages[s].phys_addr;
            page_t *pg = phys_to_page(cached_paddr);
            if (pg && pg->is_free) {
                g_shared_pages[s].vn = NULL;
                g_shared_pages[s].file_offset = 0;
                g_shared_pages[s].phys_addr = 0;
                if (pg->ref_count > 0) pg->ref_count--;
                spin_unlock_irqrestore(&g_shared_pages_lock, flags);
                vput(vn);
                return 0;
            }
            spin_unlock_irqrestore(&g_shared_pages_lock, flags);
            return cached_paddr;
        }
    }
    spin_unlock_irqrestore(&g_shared_pages_lock, flags);
    return 0;
}

void vma_shared_register(struct vnode *vn, int64_t offset, uintptr_t phys) {
    uint64_t flags = spin_lock_irqsave(&g_shared_pages_lock);
    cleanup_stale_shared_pages_locked();

    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
        if (g_shared_pages[s].vn == vn && g_shared_pages[s].file_offset == offset) {
            spin_unlock_irqrestore(&g_shared_pages_lock, flags);
            return;
        }
    }

    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
        if (g_shared_pages[s].vn == NULL) {
            g_shared_pages[s].vn = vn;
            vref(vn);
            g_shared_pages[s].file_offset = offset;
            g_shared_pages[s].phys_addr = phys;
            page_t *pg = phys_to_page(phys);
            if (pg) pg->ref_count++;
            spin_unlock_irqrestore(&g_shared_pages_lock, flags);
            return;
        }
    }

    spin_unlock_irqrestore(&g_shared_pages_lock, flags);
}

void vma_shared_unregister(struct vnode *vn, int64_t offset) {
    uint64_t flags = spin_lock_irqsave(&g_shared_pages_lock);
    for (int s = 0; s < MAX_SHARED_PAGES; s++) {
        if (g_shared_pages[s].vn == vn && g_shared_pages[s].file_offset == offset) {
            uintptr_t paddr = g_shared_pages[s].phys_addr;
            struct vnode *to_put = g_shared_pages[s].vn;
            g_shared_pages[s].vn = NULL;
            g_shared_pages[s].file_offset = 0;
            g_shared_pages[s].phys_addr = 0;
            spin_unlock_irqrestore(&g_shared_pages_lock, flags);
            page_t *pg = phys_to_page(paddr);
            if (pg && pg->ref_count > 0) pg->ref_count--;
            vput(to_put);
            return;
        }
    }
    spin_unlock_irqrestore(&g_shared_pages_lock, flags);
}

void vma_shared_cleanup(void) {
    uint64_t flags = spin_lock_irqsave(&g_shared_pages_lock);
    cleanup_stale_shared_pages_locked();
    spin_unlock_irqrestore(&g_shared_pages_lock, flags);
}

int64_t sys_mmap(uintptr_t addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    int map_type = flags & 0x0f;
    bool map_fixed = (flags & 0x10);
    bool map_anonymous = (flags & 0x20);

    if (length == 0) return -EINVAL;
    if (map_type != 0x01 && map_type != 0x02 && map_type != 0x00) return -EINVAL;
    if (fd < 0 && !map_anonymous) return -EINVAL;

    struct file *f = (fd >= 0) ? fdget(fd) : NULL;
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
                    if (mmu_is_mapped(curproc->p_vm_map, addr + off)) {
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
                    fdput(f);
                    return -ENOMEM;
                }
            }

            uint32_t fb_vma_flags = MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_READ | MMU_FLAGS_SHARED;
            struct vm_area *fb_vma = vma_alloc(start, start + aligned_fb_len, fb_vma_flags, f->f_vn, 0);
            if (fb_vma) {
                down_write(&curproc->p_vma_lock);
                vma_insert(&curproc->p_vma_root, &curproc->p_vma_list, fb_vma);
                up_write(&curproc->p_vma_lock);
            }

            fdput(f);
            return start;
        }
    if (fd >= 0 && f) fdput(f);

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
            if (mmu_is_mapped(curproc->p_vm_map, i)) {
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
                if (mmu_is_mapped(curproc->p_vm_map, i)) {
                    need_search = true;
                    break;
                }
            }
        }
    }

    if (need_search) {
        down_write(&curproc->p_vma_lock);

        uintptr_t search_start = 0x400000000000;
        uintptr_t found_addr = 0;

        while (search_start < curproc->p_mmap_base) {
            bool range_free = true;
            uintptr_t search_end = search_start + aligned_len;

            // PTE 체크 (atomic read on x86_64 — no lock needed)
            for (uintptr_t off = 0; off < aligned_len; off += PAGE_SIZE) {
                if (mmu_is_mapped(curproc->p_vm_map, search_start + off)) {
                    range_free = false;
                    search_start = ALIGN_UP(search_start + off + PAGE_SIZE, PAGE_SIZE);
                    break;
                }
            }
            if (!range_free) continue;

            // VMA 체크 (lock already held)
            struct vm_area *existing = vma_find_first(curproc->p_vma_root, search_start);
            if (existing && existing->start < search_end) {
                range_free = false;
                search_start = ALIGN_UP(existing->end, PAGE_SIZE);
            }
            if (!range_free) continue;

            found_addr = search_start;
            break;
        }

        if (found_addr != 0) {
            addr = found_addr;
        } else {
            addr = curproc->p_mmap_base;
            curproc->p_mmap_base += aligned_len;
        }
    }

    uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);
    uintptr_t end = ALIGN_UP(addr + length, PAGE_SIZE);

    struct vnode *mapped_vn = NULL;

    if (fd >= 0) {
        struct file *f_file = fdget(fd);
        if (f_file && f_file->f_vn) {
            mapped_vn = f_file->f_vn;
            vref(mapped_vn);
        }
        fdput(f_file);
    }

    uint32_t mmu_flags = MMU_FLAGS_USER;
    if (prot & 0x1) mmu_flags |= MMU_FLAGS_READ;
    if (prot & 0x2) mmu_flags |= MMU_FLAGS_WRITE;
    if (prot & 0x4) mmu_flags |= MMU_FLAGS_EXEC;
    if (flags & 0x1) mmu_flags |= MMU_FLAGS_SHARED;

    // VMA 생성
    int64_t file_offset_val = (int64_t)offset - (int64_t)(addr & (PAGE_SIZE - 1));
    struct vm_area *vma = vma_alloc(start, end, mmu_flags, mapped_vn, file_offset_val);
    if (mapped_vn) vput(mapped_vn);
    if (!vma) { if (need_search) up_write(&curproc->p_vma_lock); return -ENOMEM; }

    // VMA 설치 (lock already held for need_search)
    if (!need_search) down_write(&curproc->p_vma_lock);
    if (map_fixed) {
        vma_remove_range(&curproc->p_vma_root, &curproc->p_vma_list, start, end);
    }
    vma_insert(&curproc->p_vma_root, &curproc->p_vma_list, vma);
    up_write(&curproc->p_vma_lock);

    // dprintf("[mmap] addr=0x%lx\n", addr);
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
        mmu_unmap(map, i);
    }

    down_write(&curproc->p_vma_lock);
    vma_remove_range(&curproc->p_vma_root, &curproc->p_vma_list, start, end);
    up_write(&curproc->p_vma_lock);

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

        down_write(&curproc->p_vma_lock);
        struct vm_area *existing = vma_find(curproc->p_vma_root, start);
        if (existing && existing->start < end && existing->end > old_brk) {
            up_write(&curproc->p_vma_lock);
            return old_brk;
        }
        struct vm_area *heap_vma = vma_find(curproc->p_vma_root, old_brk - 1);
        if (heap_vma && heap_vma->end == old_brk && heap_vma->vn == NULL) {
            heap_vma->end = end;
        } else {
            struct vm_area *vma = vma_alloc(start, end,
                MMU_FLAGS_USER | MMU_FLAGS_READ | MMU_FLAGS_WRITE, NULL, 0);
            if (vma) {
                vma_insert(&curproc->p_vma_root, &curproc->p_vma_list, vma);
            } else {
                up_write(&curproc->p_vma_lock);
                return old_brk;
            }
        }
        up_write(&curproc->p_vma_lock);
    } else if (brk < old_brk) {
        uintptr_t start = ALIGN_UP(brk, PAGE_SIZE);
        uintptr_t end = ALIGN_UP(old_brk, PAGE_SIZE);
        page_table_t *map = curproc->p_vm_map;

        down_write(&curproc->p_vma_lock);
        vma_remove_range(&curproc->p_vma_root, &curproc->p_vma_list, start, end);
        up_write(&curproc->p_vma_lock);

        for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
            mmu_unmap(map, i);
        }
    }

    curproc->p_brk = brk;
    return brk;
}

static uint32_t prot_to_vma_flags(int prot) {
    uint32_t flags = MMU_FLAGS_USER;
    if (prot & 0x1) flags |= MMU_FLAGS_READ;
    if (prot & 0x2) flags |= MMU_FLAGS_WRITE;
    if (prot & 0x4) flags |= MMU_FLAGS_EXEC;
    return flags;
}

int64_t sys_mprotect(uintptr_t start, size_t len, int prot) {
    if (len == 0) return 0;
    if (!is_user_address_range((void *)start, len)) return -EINVAL;

    // dprintf("[m] start=0x%lx len=%zu\n", start, len);

    uintptr_t addr = ALIGN_DOWN(start, PAGE_SIZE);
    uintptr_t end = ALIGN_UP(start + len, PAGE_SIZE);
    if (addr >= end) return -EINVAL;
    page_table_t *map = curproc->p_vm_map;

    for (uintptr_t i = addr; i < end; i += PAGE_SIZE) {
        if (mmu_translate(map, i) == 0) {
            down_read(&curproc->p_vma_lock);
            struct vm_area *vma = vma_find(curproc->p_vma_root, i);
            up_read(&curproc->p_vma_lock);
            if (!vma) return -ENOMEM;
        }
    }

    uint32_t vma_flags = prot_to_vma_flags(prot);
    down_write(&curproc->p_vma_lock);

    struct vm_area *vma = vma_find_first(curproc->p_vma_root, addr);
    while (vma && vma->start < end) {
        if ((struct list_node *)vma == &curproc->p_vma_list) break;

        if (vma->start < addr) {
            vma = vma_split(&curproc->p_vma_root, &curproc->p_vma_list, vma, addr);
            if (!vma) break;
        }
        if (vma->end > end) {
            struct vm_area *right = vma_split(&curproc->p_vma_root, &curproc->p_vma_list, vma, end);
            if (!right) break;
        }
        vma = vma_next(vma);
    }

    vma = vma_find_first(curproc->p_vma_root, addr);
    while (vma && vma->start < end) {
        if ((struct list_node *)vma == &curproc->p_vma_list) break;
        vma->flags = (vma->flags & ~(MMU_FLAGS_READ|MMU_FLAGS_WRITE|MMU_FLAGS_EXEC)) | vma_flags;
        vma = vma_next(vma);
    }

    up_write(&curproc->p_vma_lock);

    // 페이지 우선 할당 후 매핑
    size_t num_missing = 0;
    for (uintptr_t i = addr; i < end; i += PAGE_SIZE) {
        if (mmu_translate(map, i) == 0) num_missing++;
    }

    page_t **new_pages = NULL;
    uintptr_t *new_addrs = NULL;
    int err = 0;
    if (num_missing > 0) {
        new_pages = kmalloc(sizeof(page_t *) * num_missing);
        new_addrs = kmalloc(sizeof(uintptr_t) * num_missing);
        if (!new_pages || !new_addrs) {
            kfree(new_pages);
            kfree(new_addrs);
            return -ENOMEM;
        }

        size_t idx = 0;
        for (uintptr_t i = addr; i < end && idx < num_missing; i += PAGE_SIZE) {
            if (mmu_translate(map, i) == 0) {
                page_t *pg = page_alloc(0);
                if (!pg) { err = -ENOMEM; break; }
                new_pages[idx] = pg;
                new_addrs[idx] = i;
                idx++;
            }
        }

        if (err) {
            for (size_t j = 0; j < idx; j++) page_free(new_pages[j], 0);
            kfree(new_pages);
            kfree(new_addrs);
            return err;
        }

        for (size_t idx = 0; idx < num_missing; idx++) {
            uintptr_t phys = page_to_phys(new_pages[idx]);
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            if (!mmu_map_4k(map, new_addrs[idx], phys, vma_flags | MMU_FLAGS_USER)) {
                page_free(new_pages[idx], 0);
                for (size_t j = idx + 1; j < num_missing; j++) page_free(new_pages[j], 0);
                kfree(new_pages);
                kfree(new_addrs);
                return -ENOMEM;
            }
        }
    }

    for (uintptr_t i = addr; i < end; i += PAGE_SIZE) {
        if (mmu_translate(map, i) != 0) {
            mmu_protect_page(map, i, prot);
        }
    }

    kfree(new_pages);
    kfree(new_addrs);

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

    // VMA 먼저 시도
    down_write(&curproc->p_vma_lock);
    struct vm_area *old_vma = vma_find(curproc->p_vma_root, old_addr);
    if (old_vma && old_vma->vn && (old_vma->flags & MMU_FLAGS_SHARED)) {
        orig_vn = old_vma->vn;
        orig_offset = old_vma->file_offset;
        is_shared_file = true;
    }
    up_write(&curproc->p_vma_lock);

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
                    memset(phys_to_virt(allocated_paddr), 0, PAGE_SIZE);

                    int n = orig_vn->ops->read(orig_vn, phys_to_virt(allocated_paddr), PAGE_SIZE, file_offset);
                    if (n < 0) {
                        dprintf("[sys_mremap] File read error %d at offset %ld during inplace grow\n", n, file_offset);
                        page_free(pg, 0);
                        return -EIO;
                    }

                    uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                    cleanup_stale_shared_pages_locked();
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
                page_t *pg = page_alloc(0);
                if (!pg) return -ENOMEM;
                uintptr_t paddr = page_to_phys(pg);
                memset(phys_to_virt(paddr), 0, PAGE_SIZE);
                if (!mmu_map_4k(curproc->p_vm_map, i, paddr, orig_mmu_flags)) {
                    page_free(pg, 0);
                    return -ENOMEM;
                }
            }
        }

        down_write(&curproc->p_vma_lock);
        struct vm_area *vma = vma_find(curproc->p_vma_root, old_addr);
        if (vma && vma->start == old_addr && vma->end == old_addr + aligned_old && vma->vn == NULL) {
            vma_erase(&curproc->p_vma_root, &curproc->p_vma_list, vma);
            vma->end = old_addr + aligned_new;
            vma_insert(&curproc->p_vma_root, &curproc->p_vma_list, vma);
        }
        up_write(&curproc->p_vma_lock);

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
                }
                mmu_unmap(curproc->p_vm_map, old_page_vaddr);
            } else {
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
                memset(phys_to_virt(allocated_paddr), 0, PAGE_SIZE);

                int n = orig_vn->ops->read(orig_vn, phys_to_virt(allocated_paddr), PAGE_SIZE, file_offset);
                if (n < 0) {
                    dprintf("[sys_mremap] File read error %d at offset %ld during moved grow\n", n, file_offset);
                    page_free(pg, 0);
                    return -EIO;
                }

                uint64_t lock_flags = spin_lock_irqsave(&g_shared_pages_lock);
                cleanup_stale_shared_pages_locked();
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
        }
    }

    return new_vaddr;
}
