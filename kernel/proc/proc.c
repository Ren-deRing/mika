#include <kernel/proc.h>
#include <kernel/kmem.h>
#include <kernel/kstack.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/lock.h>
#include <kernel/sched.h>
#include <kernel/mmu.h>
#include <kernel/list.h>

#include <uapi/errno.h>

#include <string.h>

pid_t next_pid = 1;
tid_t next_tid = 1;

LIST_HEAD(g_proc_list);
spinlock_t g_proc_list_lock = SPINLOCK_INITIALIZER;


struct thread* thread_create(struct proc *p, tid_t tid, void (*entry)(void *), void *arg) {
    struct thread *t = kmalloc(sizeof(struct thread));
    if (!t) return NULL;

    memset(t, 0, sizeof(struct thread));
    
    t->t_kstack = kstack_alloc();
    if (!t->t_kstack) {
        kfree(t);
        return NULL;
    }

    t->t_tid = tid;
    t->t_proc = p;
    if (!p->p_threads) {
        p->p_threads = t;
    }
    t->t_state = THREAD_READY;
    t->t_ticks = 0;
    t->t_need_resched = false;
    t->t_priority = 0;
    t->t_slice_left = 5;
    t->t_cpu = curcpu ? curcpu->id : 0;

    t->t_entry = entry;
    t->t_arg = arg;

    if (arch_thread_init(t) != 0) {
        kstack_free(t->t_kstack);
        kfree(t);
        return NULL;
    }
    
    return t;
}

extern void shm_cleanup_proc(struct proc *p);

void proc_free(struct proc *p) {
    if (!p) return;

    shm_cleanup_proc(p);

    for (int i = 0; i < MAX_FILES; i++) {
        if (p->p_fd_table[i]) {
            file_close(p->p_fd_table[i]);
            p->p_fd_table[i] = NULL;
        }
    }

    // 모든 VMA Free
    while (!list_empty(&p->p_vma_list)) {
        struct vm_area *vma = vma_first(&p->p_vma_list);
        vma_erase(&p->p_vma_root, &p->p_vma_list, vma);
        vma_free(vma);
    }

    if (p->p_vm_map && p->p_pid != 0) {
        mmu_destroy_map(p->p_vm_map);
        p->p_vm_map = NULL;
    }

    arch_proc_destroy(p);

    if (p->p_cwd) {
        vput(p->p_cwd);
    }

    kfree(p);
}

struct proc* proc_create(pid_t pid) {
    struct proc *p = kmalloc(sizeof(struct proc));
    if (!p) return NULL;

    memset(p, 0, sizeof(struct proc));
    p->p_pid = pid;
    spin_lock_init(&p->p_lock);
    spin_lock_init(&p->p_vm_lock);
    spin_lock_init(&p->p_vma_lock);
    p->p_active_cpus = 0;

    list_init(&p->p_children);
    list_init(&p->p_wait_queue);
    list_init(&p->p_vma_list);
    p->p_exit_status = 0;
    p->p_refcnt = 1;

    p->p_mmap_base = 0x400000000000;

    if (arch_proc_init(p) != 0) {
        kfree(p);
        return NULL;
    }

    if (pid == 0) {
        p->p_vm_map = mmu_get_kernel_map();
    } else {
        p->p_vm_map = mmu_create_map();
        if (!p->p_vm_map) {
            proc_free(p);
            return NULL;
        }
    }
    
    if (p->p_vm_map) {
        page_t* pg = phys_to_page(virt_to_phys(p->p_vm_map));
        if (pg) {
            pg->pg_proc = p;
        }
    }
    
    extern struct vnode *g_root_vnode;
    if (g_root_vnode) {
        vref(g_root_vnode);
        p->p_cwd = g_root_vnode;
    }

    uint64_t flags = spin_lock_irqsave(&g_proc_list_lock);
    list_add_tail(&p->p_list_link, &g_proc_list);
    spin_unlock_irqrestore(&g_proc_list_lock, flags);

    return p;
}

int proc_alloc_fd(struct proc *p, struct file *f) {
    if (!p || !f) return -EINVAL;

    spin_lock(&p->p_lock); // 락 획득
    for (int i = 0; i < MAX_FILES; i++) {
        if (p->p_fd_table[i] == NULL) {
            p->p_fd_table[i] = f;
            spin_unlock(&p->p_lock);
            return i;
        }
    }
    spin_unlock(&p->p_lock);

    return -EMFILE; // 빈자리가 없음!!
}

void proc_ref(struct proc *p) {
    if (!p) return;
    uint64_t flags = spin_lock_irqsave(&p->p_lock);
    p->p_refcnt++;
    spin_unlock_irqrestore(&p->p_lock, flags);
}

void proc_put(struct proc *p) {
    if (!p) return;
    uint64_t flags = spin_lock_irqsave(&p->p_lock);
    p->p_refcnt--;
    bool should_free = (p->p_refcnt == 0);
    spin_unlock_irqrestore(&p->p_lock, flags);

    if (should_free) {
        uint64_t g_flags = spin_lock_irqsave(&g_proc_list_lock);
        list_del(&p->p_list_link);
        spin_unlock_irqrestore(&g_proc_list_lock, g_flags);

        proc_free(p);
    }
}

struct proc* find_proc(pid_t pid) {
    struct proc *p;
    struct proc *found = NULL;
    uint64_t flags = spin_lock_irqsave(&g_proc_list_lock);
    list_for_each_entry(p, &g_proc_list, p_list_link) {
        if (p->p_pid == pid) {
            proc_ref(p);
            found = p;
            break;
        }
    }
    spin_unlock_irqrestore(&g_proc_list_lock, flags);
    return found;
}