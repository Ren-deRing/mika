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
    t0->t_state = THREAD_RUNNING;
    
    t0->t_arch_data = kmalloc_aligned(g_xsave_size, 64);
    memset(t0->t_arch_data, 0, g_xsave_size);

    curcpu->self = &cpus[0];
    curcpu->idle = t0;
    curcpu->current_thread = t0;
    curcpu->tss_rsp0 = (uintptr_t)t0->t_kstack + KSTACK_SIZE;

    return t0;
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

    memset(t->t_arch_data, 0, g_xsave_size);
    memcpy(t->t_arch_data, g_fpu_preset, g_xsave_size);
    
    uintptr_t *kstack = (uintptr_t *)((uintptr_t)t->t_kstack + KSTACK_SIZE);

    *--kstack = 0;

    *--kstack = (uintptr_t)thread_bootstrap;

    *--kstack = 0; // r15
    *--kstack = 0; // r14
    *--kstack = 0; // r13
    *--kstack = 0; // r12
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