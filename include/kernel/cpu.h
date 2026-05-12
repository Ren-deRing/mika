#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <kernel/proc.h>

#define MAX_CPUS 64
#define MAX_ISO 256

#define KSTACK_SIZE 8192
#define KSTACK_MASK (~(KSTACK_SIZE - 1))

extern uint8_t boot_stack[];

#define KMEM_NUM_CLASSES 16

struct kmem_magazine;
typedef struct kmem_magazine kmem_magazine_t;

typedef uint64_t cpu_status_t;

struct cpu {
    struct cpu *self;

    uint64_t tss_rsp0;
    uint64_t user_rsp;

    struct thread *idle;
    struct thread *current_thread;

    uint32_t id;
    uint32_t hw_id;

    uint64_t timer_ticks_per_ms; 
    bool timer_ready;

    void *arch_cpu_data;

    kmem_magazine_t* magazines[KMEM_NUM_CLASSES];
};

struct registers;
typedef void (*handler_t)(struct registers *regs, void *data);

extern struct cpu cpus[MAX_CPUS];

cpu_status_t arch_irq_save(void);
void arch_irq_restore(cpu_status_t flags);
void arch_halt(void);
void arch_pause(void);
void arch_irq_disable(void);
void arch_irq_enable(void);
struct cpu* get_this_core(void);

void arch_timer_handler(struct registers *regs, void *data);
void arch_thread_setup(struct thread *t, void (*entry)(void));

struct thread* arch_init_first_thread(void);
void arch_set_current_thread(struct thread *t);

uint64_t arch_get_system_ticks(void);

int arch_proc_init(struct proc *p);
void arch_proc_destroy(struct proc *p);
int arch_thread_init(struct thread *t);
void arch_thread_destroy(struct thread *t);
void arch_set_kernel_stack(uintptr_t kstack_top);
void arch_switch_mm(struct proc *prev, struct proc *next);

#define irq_save(flags)    do { flags = arch_irq_save(); } while(0)
#define irq_restore(flags) arch_irq_restore(flags)
#define irq_enable(void)   arch_irq_enable();
#define irq_disable(void)  arch_irq_disable();

static inline uintptr_t get_stack_pointer(void) {
    uintptr_t sp;
#if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(__aarch64__)
    asm volatile("mov %0, sp" : "=r"(sp));
#endif
    return sp;
}

#define curcpu    get_this_core()
#define curthread (curcpu->current_thread)
#define curproc   (curthread ? curthread->t_proc : NULL)

#include <kernel/proc.h>

void arch_thread_setup(struct thread *t, void (*entry)(void));