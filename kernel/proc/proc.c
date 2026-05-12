#include <kernel/proc.h>
#include <kernel/kmem.h>
#include <kernel/kstack.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/lock.h>
#include <kernel/sched.h>

#include <uapi/errno.h>

#include <string.h>

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
    t->t_state = THREAD_READY;
    t->t_ticks = 0;
    t->t_need_resched = false;

    t->t_entry = entry;
    t->t_arg = arg;

    if (arch_thread_init(t) != 0) {
        kstack_free(t->t_kstack);
        kfree(t);
        return NULL;
    }
    
    return t;
}

void proc_free(struct proc *p) {
    if (!p) return;

    for (int i = 0; i < MAX_FILES; i++) {
        if (p->p_fd_table[i]) {
            file_close(p->p_fd_table[i]);
            p->p_fd_table[i] = NULL;
        }
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
    
    extern struct vnode *g_root_vnode;
    if (g_root_vnode) {
        vref(g_root_vnode);
        p->p_cwd = g_root_vnode;
    }

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