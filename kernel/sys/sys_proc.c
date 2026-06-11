#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/exec.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <kernel/version.h>

#include <uapi/elf.h>

#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>

#include <string.h>

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

int64_t sys_clone(uint64_t flags, void *child_stack, void *ptid, void *ctid, uint64_t newtls) {
    int *parent_tid = (int *)ptid;
    int *child_tid = (int *)ctid;

    if (curthread->t_arch_data) {
        extern bool g_use_xsave;
        if (g_use_xsave) {
            uint32_t eax = 0xFFFFFFFF;
            uint32_t edx = 0xFFFFFFFF;
            asm volatile("xsaveq (%0)" 
                         : 
                         : "r"(curthread->t_arch_data), "a"(eax), "d"(edx) 
                         : "memory");
        } else {
            asm volatile("fxsave (%0)"
                         :
                         : "r"(curthread->t_arch_data)
                         : "memory");
        }
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

        child_t->t_tid = __sync_fetch_and_add(&next_tid, 1);
        child_t->t_proc = curproc;
        child_t->t_state = THREAD_READY;
        child_t->t_flags = curthread->t_flags;
        child_t->t_priority = 0;
        child_t->t_slice_left = 5;
        child_t->t_cpu = curcpu ? curcpu->id : 0;

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

        struct proc *child_p = proc_create(__sync_fetch_and_add(&next_pid, 1));
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
        if (child_p->p_vm_map) {
            page_t* pg = phys_to_page(v2p(child_p->p_vm_map));
            if (pg) {
                pg->pg_proc = child_p;
            }
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

        child_t->t_tid = __sync_fetch_and_add(&next_tid, 1);
        child_t->t_proc = child_p;
        child_p->p_threads = child_t;
        child_t->t_state = THREAD_READY;
        child_t->t_flags = parent_t->t_flags;
        child_t->t_priority = 0;
        child_t->t_slice_left = 5;
        child_t->t_cpu = curcpu ? curcpu->id : 0;

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

int64_t sys_fork(void) {
    if (curthread->t_arch_data) {
        extern bool g_use_xsave;
        if (g_use_xsave) {
            uint32_t eax = 0xFFFFFFFF;
            uint32_t edx = 0xFFFFFFFF;
            asm volatile("xsaveq (%0)" 
                         : 
                         : "r"(curthread->t_arch_data), "a"(eax), "d"(edx) 
                         : "memory");
        } else {
            asm volatile("fxsave (%0)"
                         :
                         : "r"(curthread->t_arch_data)
                         : "memory");
        }
    }

    struct proc *parent_p = curproc;
    struct thread *parent_t = curthread;

    struct proc *child_p = proc_create(__sync_fetch_and_add(&next_pid, 1));
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
    if (child_p->p_vm_map) {
        page_t* pg = phys_to_page(v2p(child_p->p_vm_map));
        if (pg) {
            pg->pg_proc = child_p;
        }
    }
    
    extern void shm_fork_copy(struct proc *parent, struct proc *child);
    shm_fork_copy(parent_p, child_p);

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

    child_t->t_tid = __sync_fetch_and_add(&next_tid, 1);
    child_t->t_proc = child_p;
    child_p->p_threads = child_t;
    child_t->t_state = THREAD_READY;
    child_t->t_flags = parent_t->t_flags;
    child_t->t_priority = 0;
    child_t->t_slice_left = 5;
    child_t->t_cpu = curcpu ? curcpu->id : 0;
    
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

int64_t sys_exit(int status) {
    if (curthread->t_clear_child_tid) {
        int zero = 0;
        if (copy_to_user(curthread->t_clear_child_tid, &zero, sizeof(int)) == 0) {
            sys_futex((uint32_t *)curthread->t_clear_child_tid, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
        }
        curthread->t_clear_child_tid = NULL;
    }

    uint64_t proc_lock_flags = spin_lock_irqsave(&curproc->p_lock);
    struct thread *prev = NULL;
    struct thread *curr = curproc->p_threads;
    while (curr) {
        if (curr == curthread) {
            if (prev) {
                prev->t_proc_next = curr->t_proc_next;
            } else {
                curproc->p_threads = curr->t_proc_next;
            }
            break;
        }
        prev = curr;
        curr = curr->t_proc_next;
    }

    bool last_thread = (curproc->p_threads == NULL);
    spin_unlock_irqrestore(&curproc->p_lock, proc_lock_flags);

    cpu_status_t irq_flags = arch_irq_save();
    (void)irq_flags;

    curthread->t_state = THREAD_ZOMBIE; 
    curthread->t_need_resched = true;

    if (last_thread) {
        curproc->p_exit_status = (int)status;
        curproc->p_state = PROC_ZOMBIE;

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
    }

    mi_switch();
    return 0;
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
    if (copy_str_from_user(kpath, user_path, 256) < 0) {
        dprintf("[sys_execve] copy_str_from_user FAILED\n");
        return -EFAULT;
    }

    char clean_path[256];
    sanitize_path(kpath, clean_path, sizeof(clean_path));
    dprintf("[sys_execve] Attempting exec: '%s'\n", clean_path);

    struct vnode *vn = NULL;
    int err = vfs_lookup(clean_path, curproc->p_cwd, &vn);
    if (err < 0) {
        dprintf("[sys_execve] vfs_lookup FAILED for '%s', err: %d\n", clean_path, err);
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
            dprintf("[sys_execve] File read FAILED, err: %d\n", n);
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
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    uintptr_t main_binary_base = (ehdr->e_type == ET_DYN) ? 0x00400000 : 0;
    uintptr_t original_entry = ehdr->e_entry + main_binary_base;
    uintptr_t entry_point = 0;
    uintptr_t brk = 0;
    uintptr_t phdr_vaddr = 0;
    uint64_t phnum = 0;
    uintptr_t interpreter_base = 0;
    
    dprintf("[sys_execve] Reading ELF header, e_type: %d, e_entry: %p\n", ehdr->e_type, ehdr->e_entry);
    page_table_t *new_map = load_elf(elf_data, &entry_point, &brk, &phdr_vaddr, &phnum, &interpreter_base);
    
    kfree(elf_data);
    
    if (!new_map) {
        dprintf("[sys_execve] load_elf FAILED! Invalid ELF Magic or Format for '%s'\n", clean_path);
        return -ENOEXEC; 
    }

    dprintf("[sys_execve] load_elf SUCCESS! entry: %p, interpreter_base: %p\n", entry_point, interpreter_base);
    uintptr_t stack_top = USER_STACK_TOP;
    uintptr_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;

    for (uintptr_t curr = stack_bottom; curr < stack_top; curr += PAGE_SIZE) {
        if (!mmu_map_demand(new_map, curr, MMU_FLAGS_USER | MMU_FLAGS_WRITE)) {
            dprintf("Memory Allocation for User Stack FAILED.\n");
            mmu_destroy_map(new_map);
            return -ENOMEM;
        }
    }

    uintptr_t final_user_rsp = setup_user_stack(new_map, USER_STACK_TOP, argv, envp, phdr_vaddr, phnum, interpreter_base, original_entry);
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
        uint64_t lock_flags = spin_lock_irqsave(&parent->p_lock);

        if (list_empty(&parent->p_children)) {
            spin_unlock_irqrestore(&parent->p_lock, lock_flags);
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
                    spin_unlock_irqrestore(&parent->p_lock, lock_flags);
                    return -EFAULT;
                }
                int wstatus_val = (exit_status & 0xff) << 8;
                if (copy_to_user(user_wstatus, &wstatus_val, sizeof(int)) < 0) {
                    spin_unlock_irqrestore(&parent->p_lock, lock_flags);
                    return -EFAULT;
                }
            }

            list_del(&found_child->p_child_link);
            spin_unlock_irqrestore(&parent->p_lock, lock_flags);

            proc_put(found_child);

            return child_pid;
        }

        if (!has_living_child) {
            spin_unlock_irqrestore(&parent->p_lock, lock_flags);
            return -ECHILD;
        }

        if (options & 1) {
            spin_unlock_irqrestore(&parent->p_lock, lock_flags);
            return 0;
        }

        struct thread *t = curthread;
        t->t_state = THREAD_WAITING;
        t->t_lock_to_release = &parent->p_lock;
        list_add_tail(&t->t_wait_node, &parent->p_wait_queue);

        arch_irq_restore(lock_flags);
        thread_yield();
    }
}

int64_t sys_getpid(void) {
    if (curproc && curproc->p_pid > 0) {
        return curproc->p_pid;
    }
    return 1;
}

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int64_t sys_uname(void *user_buf) {
    if (!is_user_address_range(user_buf, sizeof(struct utsname))) {
        return -EFAULT;
    }

    struct utsname kbuf;
    memset(&kbuf, 0, sizeof(struct utsname));
    strncpy(kbuf.sysname, __kernel_name, 64);
    strncpy(kbuf.nodename, "mika-qemu", 64);
    strncpy(kbuf.release, __kernel_release, 64);
    strncpy(kbuf.version, __kernel_version, 64);
    strncpy(kbuf.machine, __kernel_machine, 64);
    strncpy(kbuf.domainname, "mika.local", 64);

    if (copy_to_user(user_buf, &kbuf, sizeof(struct utsname)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_getuid(void) {
    return 0;
}

int64_t sys_getgid(void) {
    return 0;
}

int64_t sys_geteuid(void) {
    return 0;
}

int64_t sys_getegid(void) {
    return 0;
}

int64_t sys_lchown(const char *user_path, uid_t owner, gid_t group) {
    (void)user_path;
    (void)owner;
    (void)group;
    return 0;
}

int64_t sys_setsid(void) {
    if (!curproc) return -EINVAL;
    return curproc->p_pid;
}

int64_t sys_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    (void)ruid;
    (void)euid;
    (void)suid;
    return 0;
}
