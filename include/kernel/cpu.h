#pragma once

#include <stdint.h>
#include <stdbool.h>

struct thread;
struct proc;

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

    struct thread *prev_thread;
};

#if defined(__x86_64__)
struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t int_no;
    uint64_t err_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));
#elif defined(__aarch64__)
struct trapframe {
    uint64_t r[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
} __attribute__((packed));
#endif

typedef void (*handler_t)(struct trapframe *regs, void *data);

extern struct cpu cpus[MAX_CPUS];

cpu_status_t arch_irq_save(void);
void arch_irq_restore(cpu_status_t flags);
void arch_halt(void);
void arch_pause(void);
void arch_irq_disable(void);
void arch_irq_enable(void);
struct cpu* get_this_core(void);

void arch_timer_handler(struct trapframe *regs, void *data);
void arch_thread_setup(struct thread *t, void (*entry)(void));
void arch_sched_init(void);
void arch_trigger_resched(uint32_t cpu_id);

struct thread* arch_init_first_thread(void);
struct thread* arch_init_ap_thread(uint32_t cpu_id);
void arch_set_current_thread(struct thread *t);

uint64_t arch_get_system_ticks(void);

void arch_fpu_save(void *buf);
void arch_fpu_restore(void *buf);
uint64_t arch_get_random_seed(void);
void arch_panic_halt(void);
uint64_t arch_get_user_addr_limit(void);
uint64_t arch_get_ticks(void);

int arch_proc_init(struct proc *p);
void arch_proc_destroy(struct proc *p);
int arch_thread_init(struct thread *t);
void arch_thread_destroy(struct thread *t);
void arch_set_kernel_stack(uintptr_t kstack_top);
void arch_switch_mm(struct proc *prev, struct proc *next);
int arch_thread_fork(struct thread *child_t, struct thread *parent_t);
void arch_cpu_set_fs_base(uintptr_t addr);
void fork_child_switch_mm(void);
void arch_exec_setup_trapframe(struct trapframe *tf, uintptr_t entry, uintptr_t user_rsp);
void arch_set_kernel_stack(uintptr_t kstack_top);

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