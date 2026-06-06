#include <kernel/proc.h>
#include <kernel/kmem.h>
#include <kernel/cpu.h>
#include "x86.h"
#include <uapi/errno.h>
#include <string.h>

extern size_t g_xsave_size;
extern uint8_t g_fpu_preset[];
extern void generic_main(void);
extern void thread_bootstrap(void);

struct thread* arch_init_first_thread(void) {
    struct proc *p0 = proc_create(0);

    struct thread *t0 = kmalloc_aligned(sizeof(struct thread), 64);
    memset(t0, 0, sizeof(struct thread));

    void *new_stack = kmalloc_aligned(KSTACK_SIZE, KSTACK_SIZE);
    t0->t_kstack = new_stack;
    t0->t_tid = 0;
    t0->t_proc = p0;
    p0->p_threads = t0;
    t0->t_state = THREAD_RUNNING;
    t0->t_fs_base = 0;
    
    t0->t_arch_data = kmalloc_aligned(g_xsave_size, 64);
    memset(t0->t_arch_data, 0, g_xsave_size);

    curcpu->self = &cpus[0];
    curcpu->idle = t0;
    curcpu->current_thread = t0;
    curcpu->tss_rsp0 = (uintptr_t)t0->t_kstack + KSTACK_SIZE;

    return t0;
}

struct thread* arch_init_ap_thread(uint32_t cpu_id) {
    struct proc *p0 = find_proc(0);
    if (!p0) return NULL;

    struct thread *t = kmalloc_aligned(sizeof(struct thread), 64);
    memset(t, 0, sizeof(struct thread));

    void *new_stack = kmalloc_aligned(KSTACK_SIZE, KSTACK_SIZE);
    t->t_kstack = new_stack;
    t->t_tid = 0;
    t->t_proc = p0;

    uint64_t flags = spin_lock_irqsave(&p0->p_lock);
    t->t_proc_next = p0->p_threads;
    p0->p_threads = t;
    spin_unlock_irqrestore(&p0->p_lock, flags);

    t->t_state = THREAD_RUNNING;
    t->t_fs_base = 0;

    t->t_arch_data = kmalloc_aligned(g_xsave_size, 64);
    memset(t->t_arch_data, 0, g_xsave_size);

    struct cpu *c = &cpus[cpu_id];
    c->self = c;
    c->idle = t;
    c->current_thread = t;
    c->tss_rsp0 = (uintptr_t)t->t_kstack + KSTACK_SIZE;

    proc_put(p0);

    return t;
}


int arch_proc_init(struct proc *p) {
    struct arch_proc *ap = kmalloc(sizeof(struct arch_proc));
    if (!ap) return -ENOMEM;

    memset(ap, 0, sizeof(struct arch_proc));
    
    p->p_arch = ap;
    
    return 0;
}

void arch_proc_destroy(struct proc *p) {
    if (p->p_arch) {
        if (p->p_arch->io_bitmap) {
            kfree(p->p_arch->io_bitmap);
        }
        kfree(p->p_arch);
        p->p_arch = NULL;
    }
}

int arch_thread_init(struct thread *t) {
    t->t_arch_data = kmalloc_aligned(g_xsave_size, 64);
    if (!t->t_arch_data) {
        return -ENOMEM;
    }

    t->t_fs_base = 0;

    memset(t->t_arch_data, 0, g_xsave_size);
    memcpy(t->t_arch_data, g_fpu_preset, g_xsave_size);
    
    uintptr_t *kstack = (uintptr_t *)((uintptr_t)t->t_kstack + KSTACK_SIZE);

    *--kstack = 0;

    *--kstack = (uintptr_t)thread_bootstrap;

    *--kstack = 0; // r15
    *--kstack = 0; // r14
    *--kstack = (uintptr_t)t->t_entry;
    *--kstack = (uintptr_t)t->t_arg;
    *--kstack = 0; // rbx
    *--kstack = 0; // rbp

    t->t_context = (void *)kstack;

    return 0;
}

void arch_thread_destroy(struct thread *t) {
    if (t->t_arch_data) {
        kfree(t->t_arch_data);
        t->t_arch_data = NULL;
    }
}

void arch_switch_mm(struct proc *prev, struct proc *next) {
    if (next->p_vm_map) {
        uintptr_t next_cr3 = v2p(next->p_vm_map);
        uintptr_t curr_cr3;
        
        asm volatile("mov %%cr3, %0" : "=r"(curr_cr3));
        
        if (curr_cr3 != next_cr3) {
            asm volatile("mov %0, %%cr3" : : "r"(next_cr3) : "memory");
        }
    }
}

extern void fork_child_ret(void);
int arch_thread_fork(struct thread *child_t, struct thread *parent_t) {
    child_t->t_arch_data = kmalloc_aligned(g_xsave_size, 64);
    if (!child_t->t_arch_data) return -ENOMEM;
    memcpy(child_t->t_arch_data, parent_t->t_arch_data, g_xsave_size);

    child_t->t_fs_base = parent_t->t_fs_base;

    uintptr_t kstack_top = (uintptr_t)child_t->t_kstack + KSTACK_SIZE;

    if (!parent_t->t_trapframe) {
        kfree(child_t->t_arch_data);
        return -EINVAL;
    }

    uintptr_t tf_addr = kstack_top - sizeof(struct trapframe);

    struct trapframe *child_tf = (struct trapframe *)tf_addr;
    
    memcpy(child_tf, parent_t->t_trapframe, sizeof(struct trapframe));
    child_tf->rax = 0;
    child_t->t_trapframe = child_tf;

    uintptr_t *kstack = (uintptr_t *)child_tf;

    *--kstack = (uintptr_t)fork_child_ret;
    // *--kstack = 0;

    *--kstack = 0; // r15
    *--kstack = 0; // r14
    *--kstack = 0; // r13
    *--kstack = 0; // r12
    *--kstack = 0; // rbx
    *--kstack = 0; // rbp

    child_t->t_context = (void *)kstack;

    return 0;
}

void fork_child_switch_mm(void) {
    arch_switch_mm(NULL, curthread->t_proc);
}

void arch_exec_setup_trapframe(struct trapframe *tf, uintptr_t entry, uintptr_t user_rsp) {
    uintptr_t old_rax = tf->rax; 
    
    memset(tf, 0, sizeof(struct trapframe));

    tf->rax = old_rax;
    tf->rip = entry;
    tf->rsp = user_rsp;
    tf->cs = 0x2B;
    tf->ss = 0x23;
    tf->rcx = entry;  
    tf->r11 = 0x202;
    tf->rflags = 0x202;
}