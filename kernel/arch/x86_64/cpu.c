#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/intc.h>
#include <kernel/mmu.h>

#include <uapi/types.h>
#include <uapi/errno.h>

#include "x86.h"

#include <string.h>

volatile uint64_t g_ticks = 0;

uint8_t g_fpu_preset[4096] __attribute__((aligned(64)));
size_t g_xsave_size = 512;
bool g_use_xsave = false;

cpu_status_t arch_irq_save(void) {
    cpu_status_t flags;
    asm volatile ("pushfq; pop %0; cli" : "=rm"(flags) :: "memory");
    return flags;
}

void arch_irq_restore(cpu_status_t flags) {
    asm volatile ("push %0; popfq" : : "rm"(flags) : "memory", "cc");
}

void arch_irq_disable(void) {
    asm volatile ("cli");
}

void arch_irq_enable(void) {
    asm volatile ("sti");
}

void arch_halt(void) {
    asm volatile ("hlt");
}

void arch_pause(void) {
    asm volatile ("pause");
}

struct cpu* get_this_core(void) {
    struct cpu* ptr;
    asm volatile ("mov %%gs:0, %0" : "=r"(ptr));
    return ptr;
}

uint64_t arch_get_system_ticks(void) {
    return g_ticks;
}

void arch_timer_handler(struct trapframe *regs, void *data) {
    (void)regs; 
    (void)data;

    if (curcpu->id == 0) {
        g_ticks++;
    }
    
    sched_tick();

    g_intc->eoi(); 
}

void arch_cpu_set_fs_base(uintptr_t addr) {
    uint32_t low = (uint32_t)addr;
    uint32_t high = (uint32_t)(addr >> 32);
    
    wrmsr(MSR_FS_BASE, addr);
}

void handle_resched_ipi(struct trapframe *regs, void *data) {
    (void)regs;
    (void)data;
    if (curthread) {
        curthread->t_need_resched = true;
    }
    g_intc->eoi();
}

void handle_tlb_shootdown_ipi(struct trapframe *regs, void *data) {
    (void)regs;
    (void)data;
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    g_intc->eoi();
}

void arch_sched_init(void) {
    g_intc->register_handler(0x40, arch_timer_handler, NULL);
    g_intc->start_timer(1, 0x40);

    g_intc->register_handler(0x45, handle_resched_ipi, NULL);
    g_intc->register_handler(0x46, handle_tlb_shootdown_ipi, NULL);
}

void arch_trigger_resched(uint32_t cpu_id) {
    if (g_intc && g_intc->send_ipi) {
        struct cpu *target_cpu = &cpus[cpu_id];
        g_intc->send_ipi(target_cpu->hw_id, 0x45);
    }
}