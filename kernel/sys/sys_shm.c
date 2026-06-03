#include <uapi/sys/shm.h>
#include <uapi/errno.h>
#include <kernel/mmu.h>
#include <kernel/proc.h>
#include <kernel/lock.h>
#include <kernel/printf.h>
#include <string.h>

#define MAX_SHM_SEGMENTS 64
#define MAX_SHM_PAGES_PER_SEGMENT 1024
#define MAX_SHM_ATTACHMENTS 256
#define USER_ADDR_LIMIT  0x0000800000000000UL

struct shm_segment_internal {
    int id;                // shmid
    key_t key;
    size_t size;
    size_t num_pages;
    page_t* pages[MAX_SHM_PAGES_PER_SEGMENT];
    struct shmid_ds ds;
    bool marked_for_deletion;
    bool active;
};

struct shm_attachment_internal {
    pid_t pid;
    uintptr_t vaddr;
    int shmid;
    size_t size;
    bool active;
};

static struct shm_segment_internal shm_segments[MAX_SHM_SEGMENTS];
static struct shm_attachment_internal shm_attachments[MAX_SHM_ATTACHMENTS];
static spinlock_t shm_lock = SPINLOCK_INITIALIZER;
static int next_shmid = 1;

extern int copy_to_user(void *user_dest, const void *src, size_t n);
extern int copy_from_user(void *dest, const void *user_src, size_t n);

static inline bool is_user_address_range(const void *addr, size_t size) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    if (end < start) return false;
    if (end > USER_ADDR_LIMIT) return false;
    return true;
}

int64_t sys_shmget(key_t key, size_t size, int shmflg) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);

    if (key != IPC_PRIVATE) {
        for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
            if (shm_segments[i].active && shm_segments[i].key == key) {
                if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) {
                    spin_unlock_irqrestore(&shm_lock, flags);
                    return -EEXIST;
                }
                if (size > shm_segments[i].size) {
                    spin_unlock_irqrestore(&shm_lock, flags);
                    return -EINVAL;
                }
                int id = shm_segments[i].id;
                spin_unlock_irqrestore(&shm_lock, flags);
                return id;
            }
        }
    }

    if (!(shmflg & IPC_CREAT) && key != IPC_PRIVATE) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -ENOENT;
    }

    if (size == 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }

    size_t aligned_size = ALIGN_UP(size, PAGE_SIZE);
    size_t num_pages = aligned_size / PAGE_SIZE;

    if (num_pages > MAX_SHM_PAGES_PER_SEGMENT) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (!shm_segments[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -ENOMEM;
    }

    struct shm_segment_internal *seg = &shm_segments[slot];
    memset(seg, 0, sizeof(*seg));

    for (size_t p = 0; p < num_pages; p++) {
        page_t *pg = page_alloc(0);
        if (!pg) {
            for (size_t r = 0; r < p; r++) {
                page_free(seg->pages[r], 0);
            }
            spin_unlock_irqrestore(&shm_lock, flags);
            return -ENOMEM;
        }
        seg->pages[p] = pg;
        uintptr_t paddr = page_to_phys(pg);
        memset(p2v(paddr), 0, PAGE_SIZE);
    }

    seg->id = next_shmid++;
    seg->key = key;
    seg->size = size;
    seg->num_pages = num_pages;
    seg->active = true;

    seg->ds.shm_perm.__key = key;
    seg->ds.shm_perm.mode = shmflg & 0777;
    seg->ds.shm_perm.uid = curproc->p_uid;
    seg->ds.shm_perm.gid = curproc->p_gid;
    seg->ds.shm_perm.cuid = curproc->p_uid;
    seg->ds.shm_perm.cgid = curproc->p_gid;
    seg->ds.shm_segsz = size;
    seg->ds.shm_cpid = curproc->p_pid;
    seg->ds.shm_nattch = 0;
    seg->ds.shm_ctime = 0;

    int shmid = seg->id;
    spin_unlock_irqrestore(&shm_lock, flags);
    return shmid;
}

void* sys_shmat(int shmid, const void *shmaddr, int shmflg) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);

    struct shm_segment_internal *seg = NULL;
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (shm_segments[i].active && shm_segments[i].id == shmid) {
            seg = &shm_segments[i];
            break;
        }
    }

    if (!seg) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void*)-EINVAL;
    }

    size_t aligned_len = seg->num_pages * PAGE_SIZE;
    uintptr_t map_addr = 0;

    if (shmaddr != NULL) {
        uintptr_t req_addr = (uintptr_t)shmaddr;
        if (shmflg & SHM_RND) {
            req_addr = ALIGN_DOWN(req_addr, SHMLBA);
        } else if (req_addr % PAGE_SIZE != 0) {
            spin_unlock_irqrestore(&shm_lock, flags);
            return (void*)-EINVAL;
        }

        if (!is_user_address_range((void*)req_addr, aligned_len)) {
            spin_unlock_irqrestore(&shm_lock, flags);
            return (void*)-EINVAL;
        }

        for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
            if (mmu_translate(curproc->p_vm_map, req_addr + offset) != 0) {
                spin_unlock_irqrestore(&shm_lock, flags);
                return (void*)-EINVAL;
            }
        }
        map_addr = req_addr;
    } else {
        uintptr_t search_start = 0x500000000000;
        while (1) {
            bool range_free = true;
            if (!is_user_address_range((void*)search_start, aligned_len)) {
                spin_unlock_irqrestore(&shm_lock, flags);
                return (void*)-ENOMEM;
            }
            for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
                if (mmu_translate(curproc->p_vm_map, search_start + offset) != 0) {
                    range_free = false;
                    search_start = ALIGN_UP(search_start + offset + PAGE_SIZE, PAGE_SIZE);
                    break;
                }
            }
            if (range_free) {
                map_addr = search_start;
                break;
            }
        }
    }

    uint64_t map_flags = MMU_FLAGS_USER | MMU_FLAGS_READ;
    if (!(shmflg & SHM_RDONLY)) {
        map_flags |= MMU_FLAGS_WRITE;
    }

    for (size_t p = 0; p < seg->num_pages; p++) {
        uintptr_t phys_addr = page_to_phys(seg->pages[p]);
        if (!mmu_map_4k(curproc->p_vm_map, map_addr + (p * PAGE_SIZE), phys_addr, map_flags)) {
            for (size_t r = 0; r < p; r++) {
                mmu_unmap(curproc->p_vm_map, map_addr + (r * PAGE_SIZE));
            }
            spin_unlock_irqrestore(&shm_lock, flags);
            return (void*)-ENOMEM;
        }
    }

    int attach_slot = -1;
    for (int i = 0; i < MAX_SHM_ATTACHMENTS; i++) {
        if (!shm_attachments[i].active) {
            attach_slot = i;
            break;
        }
    }

    if (attach_slot == -1) {
        for (size_t p = 0; p < seg->num_pages; p++) {
            mmu_unmap(curproc->p_vm_map, map_addr + (p * PAGE_SIZE));
        }
        spin_unlock_irqrestore(&shm_lock, flags);
        return (void*)-ENOMEM;
    }

    shm_attachments[attach_slot].pid = curproc->p_pid;
    shm_attachments[attach_slot].vaddr = map_addr;
    shm_attachments[attach_slot].shmid = shmid;
    shm_attachments[attach_slot].size = seg->size;
    shm_attachments[attach_slot].active = true;

    seg->ds.shm_nattch++;
    seg->ds.shm_atime = 0;
    seg->ds.shm_lpid = curproc->p_pid;

    spin_unlock_irqrestore(&shm_lock, flags);
    return (void*)map_addr;
}

int64_t sys_shmdt(const void *shmaddr) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);

    uintptr_t addr = (uintptr_t)shmaddr;
    if (addr % PAGE_SIZE != 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }

    int attach_slot = -1;
    for (int i = 0; i < MAX_SHM_ATTACHMENTS; i++) {
        if (shm_attachments[i].active && 
            shm_attachments[i].pid == curproc->p_pid && 
            shm_attachments[i].vaddr == addr) {
            attach_slot = i;
            break;
        }
    }

    if (attach_slot == -1) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }

    int shmid = shm_attachments[attach_slot].shmid;

    struct shm_segment_internal *seg = NULL;
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (shm_segments[i].active && shm_segments[i].id == shmid) {
            seg = &shm_segments[i];
            break;
        }
    }

    if (!seg) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }

    for (size_t p = 0; p < seg->num_pages; p++) {
        mmu_unmap(curproc->p_vm_map, addr + (p * PAGE_SIZE));
    }

    shm_attachments[attach_slot].active = false;

    if (seg->ds.shm_nattch > 0) {
        seg->ds.shm_nattch--;
    }
    seg->ds.shm_dtime = 0;
    seg->ds.shm_lpid = curproc->p_pid;

    if (seg->marked_for_deletion && seg->ds.shm_nattch == 0) {
        for (size_t p = 0; p < seg->num_pages; p++) {
            page_free(seg->pages[p], 0);
        }
        seg->active = false;
    }

    spin_unlock_irqrestore(&shm_lock, flags);
    return 0;
}

int64_t sys_shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);

    struct shm_segment_internal *seg = NULL;
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (shm_segments[i].active && shm_segments[i].id == shmid) {
            seg = &shm_segments[i];
            break;
        }
    }

    if (!seg) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -EINVAL;
    }

    switch (cmd) {
        case IPC_RMID:
            seg->marked_for_deletion = true;
            if (seg->ds.shm_nattch == 0) {
                for (size_t p = 0; p < seg->num_pages; p++) {
                    page_free(seg->pages[p], 0);
                }
                seg->active = false;
            }
            spin_unlock_irqrestore(&shm_lock, flags);
            return 0;

        case IPC_STAT:
            if (!buf || !is_user_address_range(buf, sizeof(struct shmid_ds))) {
                spin_unlock_irqrestore(&shm_lock, flags);
                return -EFAULT;
            }
            if (copy_to_user(buf, &seg->ds, sizeof(struct shmid_ds)) < 0) {
                spin_unlock_irqrestore(&shm_lock, flags);
                return -EFAULT;
            }
            spin_unlock_irqrestore(&shm_lock, flags);
            return 0;

        default:
            spin_unlock_irqrestore(&shm_lock, flags);
            return -EINVAL;
    }
}

void shm_cleanup_proc(struct proc *p) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);

    for (int i = 0; i < MAX_SHM_ATTACHMENTS; i++) {
        if (shm_attachments[i].active && shm_attachments[i].pid == p->p_pid) {
            uintptr_t addr = shm_attachments[i].vaddr;
            int shmid = shm_attachments[i].shmid;

            struct shm_segment_internal *seg = NULL;
            for (int s = 0; s < MAX_SHM_SEGMENTS; s++) {
                if (shm_segments[s].active && shm_segments[s].id == shmid) {
                    seg = &shm_segments[s];
                    break;
                }
            }

            if (seg) {
                for (size_t pg = 0; pg < seg->num_pages; pg++) {
                    mmu_unmap(p->p_vm_map, addr + (pg * PAGE_SIZE));
                }

                if (seg->ds.shm_nattch > 0) {
                    seg->ds.shm_nattch--;
                }
                
                if (seg->marked_for_deletion && seg->ds.shm_nattch == 0) {
                    for (size_t pg = 0; pg < seg->num_pages; pg++) {
                        page_free(seg->pages[pg], 0);
                    }
                    seg->active = false;
                }
            }

            shm_attachments[i].active = false;
        }
    }

    spin_unlock_irqrestore(&shm_lock, flags);
}

void shm_fork_copy(struct proc *parent, struct proc *child) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);

    for (int i = 0; i < MAX_SHM_ATTACHMENTS; i++) {
        if (shm_attachments[i].active && shm_attachments[i].pid == parent->p_pid) {
            uintptr_t addr = shm_attachments[i].vaddr;
            int shmid = shm_attachments[i].shmid;
            size_t size = shm_attachments[i].size;

            int child_slot = -1;
            for (int j = 0; j < MAX_SHM_ATTACHMENTS; j++) {
                if (!shm_attachments[j].active) {
                    child_slot = j;
                    break;
                }
            }

            if (child_slot != -1) {
                shm_attachments[child_slot].pid = child->p_pid;
                shm_attachments[child_slot].vaddr = addr;
                shm_attachments[child_slot].shmid = shmid;
                shm_attachments[child_slot].size = size;
                shm_attachments[child_slot].active = true;

                struct shm_segment_internal *seg = NULL;
                for (int s = 0; s < MAX_SHM_SEGMENTS; s++) {
                    if (shm_segments[s].active && shm_segments[s].id == shmid) {
                        seg = &shm_segments[s];
                        break;
                    }
                }

                if (seg) {
                    seg->ds.shm_nattch++;

                    uint64_t map_flags = MMU_FLAGS_USER | MMU_FLAGS_READ | MMU_FLAGS_WRITE;

                    for (size_t p = 0; p < seg->num_pages; p++) {
                        uintptr_t phys_addr = page_to_phys(seg->pages[p]);
                        uintptr_t vaddr = addr + (p * PAGE_SIZE);

                        mmu_map_4k(child->p_vm_map, vaddr, phys_addr, map_flags);
                        mmu_map_4k(parent->p_vm_map, vaddr, phys_addr, map_flags);
                    }
                }
            }
        }
    }

    spin_unlock_irqrestore(&shm_lock, flags);
}
