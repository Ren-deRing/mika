#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/exec.h>
#include <kernel/list.h>

#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>

#include <uapi/sys/stat.h>
#include <uapi/fcntl.h>

#include <string.h>

#define USER_ADDR_LIMIT  0x0000800000000000UL

extern uint64_t get_uptime_ns(void);
extern int keyboard_queue_pop(void);

extern int64_t sys_read(int fd, void *user_buf, size_t count);
extern int64_t sys_write(int fd, const void *user_buf, size_t count);

typedef int64_t (*syscall_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static inline bool is_user_address_range(const void *addr, size_t size) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;

    if (end < start) return false;

    if (end > USER_ADDR_LIMIT) return false;

    return true;
}

int copy_to_user(void *user_dest, const void *src, size_t n) {
    page_table_t *map = curthread->t_proc->p_vm_map;
    uintptr_t dest = (uintptr_t)user_dest;
    uint8_t *s = (uint8_t *)src;
    size_t copied = 0;

    while (copied < n) {
        uintptr_t paddr = mmu_translate(map, dest + copied);
        if (!paddr) return -1;

        size_t page_offset = (dest + copied) & 0xFFF;
        size_t to_copy = 4096 - page_offset;
        if (to_copy > (n - copied)) to_copy = n - copied;

        memcpy((void *)p2v(paddr), s + copied, to_copy);
        copied += to_copy;
    }
    return 0;
}

int copy_from_user(void *dest, const void *user_src, size_t n) {
    page_table_t *map = curthread->t_proc->p_vm_map;
    uintptr_t src = (uintptr_t)user_src;
    uint8_t *d = (uint8_t *)dest;
    size_t copied = 0;

    while (copied < n) {
        uintptr_t paddr = mmu_translate(map, src + copied);
        if (!paddr) return -1;

        size_t page_offset = (src + copied) & 0xFFF;
        size_t to_copy = 4096 - page_offset;
        if (to_copy > (n - copied)) to_copy = n - copied;

        memcpy(d + copied, (void *)p2v(paddr), to_copy);
        copied += to_copy;
    }
    return 0;
}

int64_t sys_write(int fd, const void *user_buf, size_t count) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (count == 0) return 0;

    if (!is_user_address_range(user_buf, count)) return -EFAULT;

    if (fd == 1 || fd == 2) {
        char kbuf[256];
        size_t total = 0;

        while (total < count) {
            size_t to_copy = count - total;
            if (to_copy > 255) to_copy = 255;

            if (copy_from_user(kbuf, (const char *)user_buf + total, to_copy) < 0) {
                return -EFAULT;
            }

            for (size_t i = 0; i < to_copy; i++) {
                kputc(kbuf[i]);
            }
            total += to_copy;
        }
        return (int64_t)count;
    }

    return (int64_t)vfs_write(fd, user_buf, count);
}

int64_t sys_writev(int fd, const void *user_iov, int iovcnt) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -EINVAL;

    typedef struct { void *base; size_t len; } kiov_t;

    if (!is_user_address_range(user_iov, iovcnt * sizeof(kiov_t))) {
        return -EFAULT;
    }

    kiov_t *kiov = kmalloc(iovcnt * sizeof(kiov_t));
    if (!kiov) return -ENOMEM;

    if (copy_from_user(kiov, user_iov, iovcnt * sizeof(kiov_t)) < 0) {
        kfree(kiov);
        return -EFAULT;
    }

    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (kiov[i].len == 0) continue;
        int64_t r = sys_write(fd, kiov[i].base, kiov[i].len);
        if (r < 0) {
            kfree(kiov);
            return total > 0 ? total : r;
        }
        total += r;
    }

    kfree(kiov);
    return total;
}

int64_t sys_readv(int fd, const void *user_iov, int iovcnt) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -EINVAL;

    typedef struct { void *base; size_t len; } kiov_t;

    if (!is_user_address_range(user_iov, iovcnt * sizeof(kiov_t))) {
        return -EFAULT;
    }

    kiov_t *kiov = kmalloc(iovcnt * sizeof(kiov_t));
    if (!kiov) return -ENOMEM;

    if (copy_from_user(kiov, user_iov, iovcnt * sizeof(kiov_t)) < 0) {
        kfree(kiov);
        return -EFAULT;
    }

    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (kiov[i].len == 0) continue;
        int64_t r = sys_read(fd, kiov[i].base, kiov[i].len);
        if (r < 0) {
            kfree(kiov);
            return total > 0 ? total : r;
        }
        total += r;
        if (r < (int64_t)kiov[i].len) {
            break;
        }
    }

    kfree(kiov);
    return total;
}

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

static spinlock_t futex_lock = SPINLOCK_INITIALIZER;
static LIST_HEAD(g_futex_waiters);

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)uaddr2;
    (void)val3;
    (void)timeout;
    int cmd = op & 127;

    if (cmd == FUTEX_WAIT) {
        if (!is_user_address_range(uaddr, sizeof(uint32_t))) {
            return -EFAULT;
        }

        uint64_t flags = spin_lock_irqsave(&futex_lock);

        uint32_t uval;
        if (copy_from_user(&uval, uaddr, sizeof(uint32_t)) < 0) {
            spin_unlock_irqrestore(&futex_lock, flags);
            return -EFAULT;
        }

        if (uval != val) {
            spin_unlock_irqrestore(&futex_lock, flags);
            return -EAGAIN;
        }

        struct thread *t = curthread;
        t->t_state = THREAD_WAITING;
        t->t_futex_addr = (uintptr_t)uaddr;
        t->t_lock_to_release = &futex_lock;

        list_add_tail(&t->t_wait_node, &g_futex_waiters);

        thread_yield();

        if (t->t_futex_addr != 0) {
            uint64_t f = spin_lock_irqsave(&futex_lock);
            if (t->t_wait_node.next && t->t_wait_node.prev) {
                list_del(&t->t_wait_node);
            }
            t->t_futex_addr = 0;
            spin_unlock_irqrestore(&futex_lock, f);
            return -EINTR;
        }

        return 0;
    } else if (cmd == FUTEX_WAKE) {
        if (!is_user_address_range(uaddr, sizeof(uint32_t))) {
            return -EFAULT;
        }

        uint64_t flags = spin_lock_irqsave(&futex_lock);

        int woken = 0;
        struct list_node *curr = g_futex_waiters.next;
        while (curr != &g_futex_waiters) {
            if (woken >= (int)val) {
                break;
            }
            struct thread *t = list_entry(curr, struct thread, t_wait_node);
            struct list_node *next_node = curr->next;

            if (t->t_futex_addr == (uintptr_t)uaddr) {
                list_del(curr);
                t->t_futex_addr = 0;
                t->t_state = THREAD_READY;
                sched_enqueue(t);
                woken++;
            }
            curr = next_node;
        }

        spin_unlock_irqrestore(&futex_lock, flags);
        return woken;
    }

    return -ENOSYS;
}

int64_t sys_clone(uint64_t flags, void *child_stack, int *parent_tid, int *child_tid, uint64_t newtls) {
    if (curthread->t_arch_data) {
        uint32_t eax = 0xFFFFFFFF;
        uint32_t edx = 0xFFFFFFFF;
        asm volatile("xsaveq (%0)" 
                     : 
                     : "r"(curthread->t_arch_data), "a"(eax), "d"(edx) 
                     : "memory");
    }

    if (flags & CLONE_THREAD) {
        struct thread *child_t = kmalloc_aligned(sizeof(struct thread), 64);
        if (!child_t) return -ENOMEM;
        memset(child_t, 0, sizeof(struct thread));

        void *child_stack_k = kmalloc_aligned(KSTACK_SIZE, KSTACK_SIZE);
        if (!child_stack_k) {
            kfree_aligned(child_t);
            return -ENOMEM;
        }
        child_t->t_kstack = child_stack_k;

        int err = arch_thread_fork(child_t, curthread);
        if (err < 0) {
            kfree_aligned(child_stack_k);
            kfree_aligned(child_t);
            return err;
        }

        if (child_stack) {
            child_t->t_trapframe->rsp = (uintptr_t)child_stack;
        }

        child_t->t_tid = next_tid++;
        child_t->t_proc = curproc;
        child_t->t_state = THREAD_READY;
        child_t->t_flags = curthread->t_flags;

        if (flags & CLONE_SETTLS) {
            child_t->t_fs_base = newtls;
        }

        if (flags & CLONE_PARENT_SETTID) {
            if (parent_tid) {
                if (!is_user_address_range(parent_tid, sizeof(int))) return -EFAULT;
                if (copy_to_user(parent_tid, &child_t->t_tid, sizeof(int)) < 0) return -EFAULT;
            }
        }

        if (flags & CLONE_CHILD_SETTID) {
            if (child_tid) {
                if (!is_user_address_range(child_tid, sizeof(int))) return -EFAULT;
                if (copy_to_user(child_tid, &child_t->t_tid, sizeof(int)) < 0) return -EFAULT;
            }
        }

        if (flags & CLONE_CHILD_CLEARTID) {
            child_t->t_clear_child_tid = child_tid;
        }

        uint64_t lock_flags = spin_lock_irqsave(&curproc->p_lock);
        child_t->t_proc_next = curproc->p_threads;
        curproc->p_threads = child_t;
        spin_unlock_irqrestore(&curproc->p_lock, lock_flags);

        sched_enqueue(child_t);
        return child_t->t_tid;
    } else {
        struct proc *parent_p = curproc;
        struct thread *parent_t = curthread;

        struct proc *child_p = proc_create(next_pid++);
        if (!child_p) return -ENOMEM;

        child_p->p_parent = parent_p;
        child_p->p_uid = parent_p->p_uid;
        child_p->p_euid = parent_p->p_euid;
        child_p->p_gid = parent_p->p_gid;
        child_p->p_egid = parent_p->p_egid;
        child_p->p_umask = parent_p->p_umask;
        memcpy(child_p->p_name, parent_p->p_name, sizeof(parent_p->p_name));

        if (parent_p->p_cwd) {
            child_p->p_cwd = parent_p->p_cwd;
            vref(parent_p->p_cwd);
        }

        uint64_t lock_flags = spin_lock_irqsave(&parent_p->p_lock);
        for (int i = 0; i < MAX_FILES; i++) {
            if (parent_p->p_fd_table[i] != NULL) {
                child_p->p_fd_table[i] = parent_p->p_fd_table[i];
                child_p->p_fd_table[i]->f_refcnt++; 
            } else {
                child_p->p_fd_table[i] = NULL;
            }
        }
        spin_unlock_irqrestore(&parent_p->p_lock, lock_flags);

        child_p->p_vm_map = mmu_clone_map(parent_p->p_vm_map);
        if (!child_p->p_vm_map) {
            goto err_proc;
        }
        child_p->p_entry = parent_p->p_entry;
        child_p->p_stack_top = parent_p->p_stack_top;
        child_p->p_brk = parent_p->p_brk;
        child_p->p_mmap_base = parent_p->p_mmap_base;

        struct thread *child_t = kmalloc_aligned(sizeof(struct thread), 64);
        if (!child_t) {
            goto err_vm_map;
        }
        memset(child_t, 0, sizeof(struct thread));

        void *child_stack_k = kmalloc_aligned(KSTACK_SIZE, KSTACK_SIZE);
        if (!child_stack_k) {
            goto err_thread;
        }
        child_t->t_kstack = child_stack_k;

        int err = arch_thread_fork(child_t, parent_t);
        if (err < 0) {
            goto err_stack;
        }

        if (child_stack) {
            child_t->t_trapframe->rsp = (uintptr_t)child_stack;
        }

        child_t->t_tid = next_tid++;
        child_t->t_proc = child_p;
        child_p->p_threads = child_t;
        child_t->t_state = THREAD_READY;
        child_t->t_flags = parent_t->t_flags;

        if (flags & CLONE_SETTLS) {
            child_t->t_fs_base = newtls;
        }

        if (flags & CLONE_PARENT_SETTID) {
            if (parent_tid) {
                if (!is_user_address_range(parent_tid, sizeof(int))) goto err_stack;
                if (copy_to_user(parent_tid, &child_t->t_tid, sizeof(int)) < 0) goto err_stack;
            }
        }

        if (flags & CLONE_CHILD_SETTID) {
            if (child_tid) {
                if (!is_user_address_range(child_tid, sizeof(int))) goto err_stack;
                if (copy_to_user(child_tid, &child_t->t_tid, sizeof(int)) < 0) goto err_stack;
            }
        }

        if (flags & CLONE_CHILD_CLEARTID) {
            child_t->t_clear_child_tid = child_tid;
        }

        uint64_t lock_flags2 = spin_lock_irqsave(&parent_p->p_lock);
        list_add_tail(&child_p->p_child_link, &parent_p->p_children);
        spin_unlock_irqrestore(&parent_p->p_lock, lock_flags2);

        sched_enqueue(child_t);
        return child_p->p_pid;

    err_stack:
        kfree_aligned(child_stack_k);
    err_thread:
        kfree_aligned(child_t);
    err_vm_map:
        mmu_destroy_map(child_p->p_vm_map);
        child_p->p_vm_map = NULL;
    err_proc:
        proc_put(child_p);
        return -ENOMEM;
    }
}

int64_t sys_exit(int64_t status) {
    if (curthread->t_clear_child_tid) {
        int zero = 0;
        if (copy_to_user(curthread->t_clear_child_tid, &zero, sizeof(int)) == 0) {
            sys_futex((uint32_t *)curthread->t_clear_child_tid, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
        }
        curthread->t_clear_child_tid = NULL;
    }

    cpu_status_t flags = arch_irq_save();
    (void)flags;

    curproc->p_exit_status = (int)status;
    curproc->p_state = PROC_ZOMBIE;
    curthread->t_state = THREAD_ZOMBIE; 
    curthread->t_need_resched = true;

    struct proc *parent = curproc->p_parent;
    if (parent) {
        spin_lock(&parent->p_lock);
        while (!list_empty(&parent->p_wait_queue)) {
            struct thread *waiter = list_first_entry(&parent->p_wait_queue, struct thread, t_wait_node);
            list_del(&waiter->t_wait_node);
            waiter->t_state = THREAD_READY;
            sched_enqueue(waiter);
        }
        spin_unlock(&parent->p_lock);
    }

    mi_switch();
    return 0;
}

int64_t sys_mmap(uintptr_t addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    (void)flags;
    (void)offset;

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
                    for (uintptr_t offset = 0; offset < aligned_fb_len; offset += PAGE_SIZE) {
                        if (mmu_translate(curproc->p_vm_map, addr + offset) != 0) {
                            range_free = false;
                            addr = ALIGN_UP(addr + offset + PAGE_SIZE, PAGE_SIZE);
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
            dprintf("[KERNEL sys_mmap fb0] fb_addr=%p -> phys_fb=%p\n", g_boot_info.fb.fb_addr, phys_fb);
            uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);

            for (uintptr_t i = 0; i < aligned_fb_len; i += PAGE_SIZE) {
                uint32_t mmu_flags = MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_READ;
                if (!mmu_map_4k(curproc->p_vm_map, start + i, phys_fb + i, mmu_flags)) {
                    return -ENOMEM;
                }
            }
            return start;
        }
    }

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    bool need_search = (addr == 0);

    if (addr != 0) {
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
            for (uintptr_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
                if (mmu_translate(curproc->p_vm_map, search_start + offset) != 0) {
                    range_free = false;
                    search_start = ALIGN_UP(search_start + offset + PAGE_SIZE, PAGE_SIZE);
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
            addr = curproc->p_mmap_base;
            curproc->p_mmap_base += aligned_len;
        }
    }

    uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);
    uintptr_t end = ALIGN_UP(addr + length, PAGE_SIZE);

    for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
        page_t *pg = page_alloc(0);
        if (!pg) return -ENOMEM;

        uintptr_t paddr = page_to_phys(pg);
        memset(p2v(paddr), 0, PAGE_SIZE);

        uint32_t mmu_flags = MMU_FLAGS_USER;
        if (prot & 0x2) mmu_flags |= MMU_FLAGS_WRITE; // PROT_WRITE

        if (!mmu_map_4k(curproc->p_vm_map, i, paddr, mmu_flags)) {
            page_free(pg, 0);
            return -ENOMEM;
        }
    }

    return addr;
}

int64_t sys_munmap(uintptr_t addr, size_t length) {
    if (length == 0) return -EINVAL;
    if (!is_user_address_range((void *)addr, length)) return -EINVAL;

    uintptr_t start = ALIGN_DOWN(addr, PAGE_SIZE);
    uintptr_t end = ALIGN_UP(addr + length, PAGE_SIZE);
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

int64_t sys_open(const char *user_path, int flags, mode_t mode) {
    char kpath[256];
    if (copy_from_user(kpath, user_path, 256) < 0) return -EFAULT;
    kpath[255] = '\0';

    int fd_out = -1;
    int err = vfs_open(kpath, flags, mode, &fd_out);
    dprintf("[KERNEL sys_open] path='%s', flags=%x, mode=%x -> fd=%d (err=%d)\n", kpath, flags, mode, fd_out, err);
    if (err < 0) return (int64_t)err;
    return (int64_t)fd_out;
}

int64_t sys_close(int fd) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;

    struct file *f = curproc->p_fd_table[fd];
    if (!f) return -EBADF;

    curproc->p_fd_table[fd] = NULL;

    file_close(f); 
    return 0;
}

int64_t sys_lseek(int fd, off_t offset, int whence) {
    int64_t res = (int64_t)vfs_lseek(fd, offset, whence);

    return res;
}

int64_t sys_read(int fd, void *user_buf, size_t count) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (count == 0) return 0;
    if (!is_user_address_range(user_buf, count)) return -EFAULT;

    struct file *f = curproc->p_fd_table[fd];
    if (f && f->f_vn && strcmp(f->f_vn->v_name, "kbd") == 0) {
        size_t copied = 0;
        char *ubuf = (char *)user_buf;

        while (copied < count) {
            int sc = keyboard_queue_pop();
            if (sc < 0) break;

            uint8_t sc8 = (uint8_t)sc;
            if (copy_to_user(ubuf + copied, &sc8, 1) < 0) {
                return -EFAULT;
            }
            copied++;
        }
        return (int64_t)copied;
    }

    char kbuf[4096];
    size_t total = 0;

    while (total < count) {
        size_t to_copy = count - total;
        if (to_copy > sizeof(kbuf)) to_copy = sizeof(kbuf);

        int n = vfs_read(fd, kbuf, to_copy);

        if (n < 0) return (total == 0) ? n : (int64_t)total;
        if (n == 0) break;

        if (copy_to_user((char *)user_buf + total, kbuf, n) < 0) {
            return -EFAULT;
        }
        total += n;
    }

    return (int64_t)total;
}

int sys_fork(void) {
    if (curthread->t_arch_data) {
        uint32_t eax = 0xFFFFFFFF;
        uint32_t edx = 0xFFFFFFFF;
        asm volatile("xsaveq (%0)" 
                     : 
                     : "r"(curthread->t_arch_data), "a"(eax), "d"(edx) 
                     : "memory");
    }

    struct proc *parent_p = curproc;
    struct thread *parent_t = curthread;

    struct proc *child_p = proc_create(next_pid++);
    if (!child_p) return -ENOMEM;

    child_p->p_parent = parent_p;
    child_p->p_uid = parent_p->p_uid;
    child_p->p_euid = parent_p->p_euid;
    child_p->p_gid = parent_p->p_gid;
    child_p->p_egid = parent_p->p_egid;
    child_p->p_umask = parent_p->p_umask;
    memcpy(child_p->p_name, parent_p->p_name, sizeof(parent_p->p_name));

    if (parent_p->p_cwd) {
        child_p->p_cwd = parent_p->p_cwd;
        vref(parent_p->p_cwd);
    }

    uint64_t lock_flags = spin_lock_irqsave(&parent_p->p_lock);
    for (int i = 0; i < MAX_FILES; i++) {
        if (parent_p->p_fd_table[i] != NULL) {
            child_p->p_fd_table[i] = parent_p->p_fd_table[i];
            child_p->p_fd_table[i]->f_refcnt++; 
        } else {
            child_p->p_fd_table[i] = NULL;
        }
    }
    spin_unlock_irqrestore(&parent_p->p_lock, lock_flags);

    child_p->p_vm_map = mmu_clone_map(parent_p->p_vm_map);
    if (!child_p->p_vm_map) {
        goto err_proc;
    }
    child_p->p_entry = parent_p->p_entry;
    child_p->p_stack_top = parent_p->p_stack_top;
    child_p->p_brk = parent_p->p_brk;
    child_p->p_mmap_base = parent_p->p_mmap_base;

    struct thread *child_t = kmalloc_aligned(sizeof(struct thread), 64);
    if (!child_t) {
        goto err_vm_map;
    }
    memset(child_t, 0, sizeof(struct thread));

    void *child_stack = kmalloc_aligned(KSTACK_SIZE, KSTACK_SIZE);
    if (!child_stack) {
        goto err_thread;
    }
    child_t->t_kstack = child_stack;

    int err = arch_thread_fork(child_t, parent_t);
    if (err < 0) {
        goto err_stack;
    }

    child_t->t_tid = next_tid++;
    child_t->t_proc = child_p;
    child_p->p_threads = child_t;
    child_t->t_state = THREAD_READY;
    child_t->t_flags = parent_t->t_flags;
    
    uint64_t lock_flags2 = spin_lock_irqsave(&parent_p->p_lock);
    list_add_tail(&child_p->p_child_link, &parent_p->p_children);
    spin_unlock_irqrestore(&parent_p->p_lock, lock_flags2);

    sched_enqueue(child_t);
    return child_p->p_pid;

err_stack:
    kfree_aligned(child_stack);
err_thread:
    kfree_aligned(child_t);
err_vm_map:
    mmu_destroy_map(child_p->p_vm_map);
    child_p->p_vm_map = NULL;
err_proc:
    proc_put(child_p);
    return -ENOMEM;
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

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

int64_t sys_arch_prctl(int code, uintptr_t addr) {
    if (code == ARCH_SET_FS) {
        if (!is_user_address_range((void *)addr, sizeof(void *))) {
            return -EFAULT;
        }

        arch_cpu_set_fs_base(addr);

        curthread->t_fs_base = addr;

        return 0;
    }

    return -EINVAL;
}

int64_t sys_set_tid_address(int *tidptr) {
    if (tidptr) {
        if (!is_user_address_range(tidptr, sizeof(int))) return -EFAULT;
    }
    
    if (curthread->t_tid == 0) curthread->t_tid = 1; 
    return (int64_t)curthread->t_tid;
}

int64_t sys_execve(const char *user_path, char *const argv[], char *const envp[]) {
    char kpath[256];
    if (copy_from_user(kpath, user_path, 256) < 0) {
        return -EFAULT;
    }

    dprintf("[KERNEL sys_execve] path='%s'\n", kpath);
    if (argv) {
        int i = 0;
        while (argv[i]) {
            dprintf("[KERNEL sys_execve] argv[%d] = '%s' (ptr=%p)\n", i, argv[i], argv[i]);
            i++;
        }
    } else {
        dprintf("[KERNEL sys_execve] argv is NULL\n");
    }

    struct vnode *vn = NULL;
    int err = vfs_lookup(kpath, curproc->p_cwd, &vn);
    if (err < 0) {
        return err;
    }

    size_t allocated_size = 64 * 1024;
    void *elf_data = kmalloc(allocated_size);
    size_t file_size = 0;

    while (1) {
        if (file_size >= allocated_size) {
            size_t new_size = allocated_size * 2;
            void *new_buf = kmalloc(new_size);
            memcpy(new_buf, elf_data, file_size);
            kfree(elf_data);
            elf_data = new_buf;
            allocated_size = new_size;
        }
        size_t space_left = allocated_size - file_size;
        
        int n = vn->ops->read(vn, (void *)((uintptr_t)elf_data + file_size), space_left, file_size);
        
        if (n < 0) {
            kfree(elf_data);
            vput(vn);
            return n;
        }
        if (n == 0) {
            break;
        }

        file_size += n;
    }

    vput(vn); 

    uintptr_t entry_point = 0;
    uintptr_t brk = 0;
    uintptr_t phdr_vaddr = 0;
    uint64_t phnum = 0;
    page_table_t *new_map = load_elf(elf_data, &entry_point, &brk, &phdr_vaddr, &phnum);
    
    kfree(elf_data);
    
    if (!new_map) {
        dprintf("load_elf FAILED! Invalid ELF Magic or Format.\n");
        return -ENOEXEC; 
    }

    uintptr_t stack_top = USER_STACK_TOP;
    uintptr_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;

    for (uintptr_t curr = stack_bottom; curr < stack_top; curr += PAGE_SIZE) {
        page_t *pg = page_alloc(0);
        if (!pg) {
            dprintf("Memory Allocation for User Stack FAILED.\n");
            mmu_destroy_map(new_map);
            return -ENOMEM;
        }
        uintptr_t paddr = page_to_phys(pg);
        memset(p2v(paddr), 0, PAGE_SIZE);
        mmu_map(new_map, curr, paddr, MMU_FLAGS_USER | MMU_FLAGS_WRITE);
    }

    uintptr_t final_user_rsp = setup_user_stack(new_map, USER_STACK_TOP, argv, envp, phdr_vaddr, phnum);
    if (final_user_rsp == 0 || (final_user_rsp % 16) != 0) {
        dprintf("setup_user_stack FAILED. Misaligned RSP: %p\n", final_user_rsp);
        mmu_destroy_map(new_map);
        return -EFAULT;
    }

    page_table_t *old_map = curproc->p_vm_map;
    curproc->p_vm_map = new_map;
    curproc->p_entry = entry_point;
    curproc->p_brk = ALIGN_UP(brk, PAGE_SIZE);
    curproc->p_stack_top = USER_STACK_TOP;

    arch_switch_mm(NULL, curproc);
    mmu_destroy_map(old_map); 

    arch_set_kernel_stack((uintptr_t)curthread->t_kstack + KSTACK_SIZE);
    arch_exec_setup_trapframe(curthread->t_trapframe, entry_point, final_user_rsp);

    curthread->t_trapframe->rax = 0;

    return curthread->t_trapframe->rax; 
}

int64_t sys_wait4(pid_t pid, int *user_wstatus, int options, void *user_rusage) {
    (void)user_rusage;
    struct proc *parent = curproc;

    while (1) {
        uint64_t flags = spin_lock_irqsave(&parent->p_lock);

        if (list_empty(&parent->p_children)) {
            spin_unlock_irqrestore(&parent->p_lock, flags);
            return -ECHILD;
        }

        struct proc *found_child = NULL;
        bool has_living_child = false;

        struct proc *child;
        list_for_each_entry(child, &parent->p_children, p_child_link) {
            if (pid == -1 || child->p_pid == pid) {
                if (child->p_state == PROC_ZOMBIE) {
                    found_child = child;
                    break;
                }
                has_living_child = true;
            }
        }

        if (found_child) {
            pid_t child_pid = found_child->p_pid;
            int exit_status = found_child->p_exit_status;

            if (user_wstatus != NULL) {
                if (!is_user_address_range(user_wstatus, sizeof(int))) {
                    spin_unlock_irqrestore(&parent->p_lock, flags);
                    return -EFAULT;
                }
                int wstatus_val = (exit_status & 0xff) << 8;
                if (copy_to_user(user_wstatus, &wstatus_val, sizeof(int)) < 0) {
                    spin_unlock_irqrestore(&parent->p_lock, flags);
                    return -EFAULT;
                }
            }

            list_del(&found_child->p_child_link);
            spin_unlock_irqrestore(&parent->p_lock, flags);

            proc_put(found_child);

            return child_pid;
        }

        if (!has_living_child) {
            spin_unlock_irqrestore(&parent->p_lock, flags);
            return -ECHILD;
        }

        if (options & 1) {
            spin_unlock_irqrestore(&parent->p_lock, flags);
            return 0;
        }

        struct thread *t = curthread;
        t->t_state = THREAD_WAITING;
        t->t_lock_to_release = &parent->p_lock;
        list_add_tail(&t->t_wait_node, &parent->p_wait_queue);

        arch_irq_restore(flags);
        thread_yield();
    }
}

int64_t sys_fstat(int fd, struct stat *user_statbuf) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!is_user_address_range(user_statbuf, sizeof(struct stat))) return -EFAULT;

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    if (fd == 0 || fd == 1 || fd == 2) {
        kst.st_mode = S_IFCHR | 0666; 
        kst.st_blksize = 1024;
        kst.st_blocks = 0;
        kst.st_size = 0;
    } else {
        struct file *f = curproc->p_fd_table[fd];
        if (!f || !f->f_vn) return -EBADF;

        kst.st_mode = S_IFREG | 0644;
        kst.st_blksize = 4096;
        kst.st_size = 0;
    }

    if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_newfstatat(int dirfd, const char *user_path, struct stat *user_statbuf, int flags) {
    if (!is_user_address_range(user_statbuf, sizeof(struct stat))) return -EFAULT;

    if (flags & AT_EMPTY_PATH) {
        return sys_fstat(dirfd, user_statbuf);
    }

    char kpath[256];
    if (copy_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    if (strlen(kpath) == 0) {
        return sys_fstat(dirfd, user_statbuf);
    }

    struct vnode *vn = NULL;
    int err = vfs_lookup(kpath, curproc->p_cwd, &vn);
    if (err < 0) return err;

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    kst.st_mode = S_IFREG | 0644; 
    kst.st_blksize = 4096;
    kst.st_size = 0;

    vput(vn);

    if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_getpid(void) {
    if (curproc && curproc->p_pid > 0) {
        return curproc->p_pid;
    }
    return 1;
}

int64_t sys_ioctl(int fd, uint64_t request, void *arg) {
    if (fd >= 0 && fd < MAX_FILES) {
        struct file *f = curproc->p_fd_table[fd];
        if (f && f->f_vn && strcmp(f->f_vn->v_name, "fb0") == 0) {
            if (request == 0x4601) { // FBIOGET_INFO
                struct {
                    uint32_t width;
                    uint32_t height;
                    uint32_t pitch;
                    uint32_t bpp;
                } info = {
                    g_boot_info.fb.width,
                    g_boot_info.fb.height,
                    g_boot_info.fb.pitch,
                    g_boot_info.fb.bpp
                };
                if (!is_user_address_range(arg, sizeof(info))) return -EFAULT;
                if (copy_to_user(arg, &info, sizeof(info)) < 0) return -EFAULT;
                return 0;
            }
        }
    }

    if (request == 0x5413) {
        if (!is_user_address_range(arg, 8)) {
            return -EFAULT;
        }

        unsigned short fake_winsize[4] = { 25, 80, 0, 0 };

        if (copy_to_user(arg, fake_winsize, 8) < 0) {
            return -EFAULT;
        }

        return 0;
    }

    return -ENOTTY;
}

int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;

    struct file *f = curproc->p_fd_table[fd];
    if (!f) return -EBADF;

    dprintf("[KERNEL sys_fcntl] fd=%d, cmd=%d, arg=%llu\n", fd, cmd, arg);

    if (cmd == 3) { // F_GETFL
        return (int64_t)f->f_flags;
    }
    if (cmd == 4) { // F_SETFL
        f->f_flags = (f->f_flags & ~0xFFFFFFF) | (arg & 0xFFFFFFF);
        return 0;
    }
    if (cmd == 1) { // F_GETFD
        return 0;
    }
    if (cmd == 2) { // F_SETFD
        return 0;
    }

    return 0;
}

int64_t sys_getrlimit(int resource, void *user_rlim) {
    struct { uint64_t cur, max; } rl = { 8*1024*1024UL, (uint64_t)-1 };
    if (!is_user_address_range(user_rlim, sizeof(rl))) return -EFAULT;
    if (copy_to_user(user_rlim, &rl, sizeof(rl)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_clock_gettime(int clk_id, void *user_tp) {
    (void)clk_id;
    struct { int64_t tv_sec; int64_t tv_nsec; } tp;
    uint64_t ns = get_uptime_ns();
    tp.tv_sec = ns / 1000000000ULL;
    tp.tv_nsec = ns % 1000000000ULL;

    if (!is_user_address_range(user_tp, sizeof(tp))) return -EFAULT;
    if (copy_to_user(user_tp, &tp, sizeof(tp)) < 0) return -EFAULT;
    return 0;
}

void handle_signal_dispatch(struct trapframe *tf, int sig, struct sigaction *act) {
    uintptr_t user_rsp = tf->rsp;

    uintptr_t frame_addr = (user_rsp - sizeof(struct sigframe)) & ~0xF;

    struct sigframe frame;
    memset(&frame, 0, sizeof(struct sigframe));

    memcpy(&frame.sf_tf, tf, sizeof(struct trapframe));
    frame.sf_oldmask = curthread->t_sig_mask;

    // 트램폴린 준비 (mov rax, 15; syscall)
    frame.sf_trampoline[0] = 0x48; // mov rax, 15
    frame.sf_trampoline[1] = 0xc7;
    frame.sf_trampoline[2] = 0xc0;
    frame.sf_trampoline[3] = 0x0f;
    frame.sf_trampoline[4] = 0x00;
    frame.sf_trampoline[5] = 0x00;
    frame.sf_trampoline[6] = 0x00;
    frame.sf_trampoline[7] = 0x0f; // syscall
    frame.sf_trampoline[8] = 0x05;

    if (copy_to_user((void *)frame_addr, &frame, sizeof(struct sigframe)) < 0) {
        sys_exit(128 + SIGSEGV);
        return;
    }

    tf->rip = (uintptr_t)act->sa_handler;
    tf->rdi = (uint64_t)sig;
    tf->rsp = frame_addr;

    uintptr_t ret_addr = act->sa_restorer ? (uintptr_t)act->sa_restorer : (frame_addr + offsetof(struct sigframe, sf_trampoline));
    tf->rsp -= 8;
    if (copy_to_user((void *)tf->rsp, &ret_addr, sizeof(uintptr_t)) < 0) {
        sys_exit(128 + SIGSEGV);
        return;
    }

    curthread->t_sig_mask |= act->sa_mask;
    curthread->t_in_sighandler = true;

    curthread->t_sig_pending &= ~(1ULL << (sig - 1));
}

void check_signals(struct trapframe *tf) {
    if (!curthread || !curthread->t_proc) return;
    if (curthread->t_tid == 0) return;

    if ((tf->cs & 3) != 3) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&curthread->t_proc->p_lock);

    uint64_t pending = curthread->t_sig_pending & ~curthread->t_sig_mask;
    if (pending == 0) {
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    }

    int sig = 0;
    for (int i = 0; i < NSIG; i++) {
        if (pending & (1ULL << i)) {
            sig = i + 1;
            break;
        }
    }

    if (sig == 0) {
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    }

    struct sigaction act = curthread->t_sig_actions[sig - 1];

    if (act.sa_handler == SIG_DFL) {
        if (sig == SIGKILL || sig == SIGTERM || sig == SIGINT || sig == SIGQUIT || sig == SIGHUP || 
            sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGBUS || sig == SIGABRT) {
            
            spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
            dprintf("[SIGNAL] Process %d killed by signal %d\n", curthread->t_proc->p_pid, sig);
            sys_exit(128 + sig);
            return;
        }
        curthread->t_sig_pending &= ~(1ULL << (sig - 1));
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    } else if (act.sa_handler == SIG_IGN) {
        curthread->t_sig_pending &= ~(1ULL << (sig - 1));
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    } else {
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        handle_signal_dispatch(tf, sig, &act);
        return;
    }
}

int64_t sys_rt_sigaction(int sig, const void *act, void *oact, size_t sigsetsize) {
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }
    if (sigsetsize != sizeof(sigset_t)) {
        return -EINVAL;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -EINVAL;
    }

    if (oact) {
        if (!is_user_address_range(oact, sizeof(struct sigaction))) {
            return -EFAULT;
        }
        if (copy_to_user(oact, &curthread->t_sig_actions[sig - 1], sizeof(struct sigaction)) < 0) {
            return -EFAULT;
        }
    }

    if (act) {
        if (!is_user_address_range(act, sizeof(struct sigaction))) {
            return -EFAULT;
        }
        struct sigaction new_act;
        if (copy_from_user(&new_act, act, sizeof(struct sigaction)) < 0) {
            return -EFAULT;
        }
        curthread->t_sig_actions[sig - 1] = new_act;
    }

    return 0;
}

int64_t sys_rt_sigreturn(void) {
    uintptr_t frame_addr = curthread->t_trapframe->rsp;

    struct sigframe frame;
    if (copy_from_user(&frame, (void *)frame_addr, sizeof(struct sigframe)) < 0) {
        sys_exit(128 + SIGSEGV);
        return -EFAULT;
    }

    if ((frame.sf_tf.cs & 3) != 3 || (frame.sf_tf.ss & 3) != 3) {
        dprintf("Attempted privilege escalation via sigreturn! PID: %d\n", curthread->t_proc->p_pid);
        sys_exit(128 + SIGSEGV);
        return -EPERM;
    }

    memcpy(curthread->t_trapframe, &frame.sf_tf, sizeof(struct trapframe));
    curthread->t_sig_mask = frame.sf_oldmask;
    curthread->t_in_sighandler = false;

    return curthread->t_trapframe->rax;
}

int64_t sys_kill(pid_t pid, int sig) {
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }

    struct proc *p = find_proc(pid);
    if (!p) {
        return -ESRCH;
    }

    struct thread *t = p->p_threads;
    if (!t) {
        proc_put(p);
        return -ESRCH;
    }

    uint64_t flags = spin_lock_irqsave(&p->p_lock);
    t->t_sig_pending |= (1ULL << (sig - 1));
    spin_unlock_irqrestore(&p->p_lock, flags);

    thread_signal_wakeup(t);

    proc_put(p);
    return 0;
}


int64_t sys_rt_sigprocmask(int how, const void *set, void *oset, size_t sigsetsize) {
    (void)how; (void)set; (void)sigsetsize;
    if (oset && is_user_address_range(oset, sigsetsize)) {
        uint64_t zero = 0;
        copy_to_user(oset, &zero, sizeof(zero));
    }
    return 0;
}

static syscall_t syscall_table[] = {
    [0] = (syscall_t)sys_read,
    [1] = (syscall_t)sys_write,
    [2] = (syscall_t)sys_open,
    [3] = (syscall_t)sys_close,
    [5] = (syscall_t)sys_fstat,
    [8] = (syscall_t)sys_lseek,
    [9]  = (syscall_t)sys_mmap,
    [10] = (syscall_t)sys_mprotect,
    [11] = (syscall_t)sys_munmap,
    [12] = (syscall_t)sys_brk,
    [13] = (syscall_t)sys_rt_sigaction,
    [14] = (syscall_t)sys_rt_sigprocmask,
    [15] = (syscall_t)sys_rt_sigreturn,
    [16] = (syscall_t)sys_ioctl,
    [19] = (syscall_t)sys_readv,
    [20] = (syscall_t)sys_writev,
    [39] = (syscall_t)sys_getpid,
    [56] = (syscall_t)sys_clone,
    [57] = (syscall_t)sys_fork,
    [59] = (syscall_t)sys_execve,
    [60] = (syscall_t)sys_exit,
    [61] = (syscall_t)sys_wait4,
    [62] = (syscall_t)sys_kill,
    // [63]  = (syscall_t)sys_uname,
    [72] = (syscall_t)sys_fcntl,
    [97]  = (syscall_t)sys_getrlimit,
    [158] = (syscall_t)sys_arch_prctl,
    [202] = (syscall_t)sys_futex,
    [218] = (syscall_t)sys_set_tid_address,
    [228] = (syscall_t)sys_clock_gettime,
    [231] = (syscall_t)sys_exit, // sys_exit_group
    [262] = (syscall_t)sys_newfstatat,
};

int64_t do_syscall_handler(struct trapframe *tf) {
    curthread->t_trapframe = tf;

    uint64_t num = tf->rax;

    uint64_t a1 = tf->rdi;
    uint64_t a2 = tf->rsi;
    uint64_t a3 = tf->rdx;
    uint64_t a4 = tf->r10;
    uint64_t a5 = tf->r8;
    uint64_t a6 = tf->r9;

    if (num >= sizeof(syscall_table)/sizeof(syscall_t) ||
        !syscall_table[num]) {
        tf->rax = -ENOSYS;
        return tf->rax;
    }

    int64_t ret =
        syscall_table[num](a1, a2, a3, a4, a5, a6);

    tf->rax = ret;

    check_signals(tf);

    return ret;
}