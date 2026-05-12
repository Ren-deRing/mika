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

void arch_timer_handler(struct registers *regs, void *data) {
    (void)regs; 
    (void)data;

    g_intc->eoi();

    g_ticks++;
    
    sched_tick(); 
}