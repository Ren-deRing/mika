#pragma once

#include <stdint.h>

#include <kernel/cpu.h>

struct interrupt_controller {
    const char* name;
    
    int (*get_cpu_count)(void);
    uint32_t (*get_cpu_id)(int index);
    uint32_t (*get_hw_id)(int cpu_index);

    void (*init_local)(void);
    void (*eoi)(void);
    
    void (*send_ipi)(uint32_t target_lapic_id, uint8_t vector);

    void (*register_handler)(uint8_t vector, handler_t handler, void *data);
    void (*map_irq)(uint8_t irq, uint32_t target_cpu, uint8_t vector);

    void (*mask)(uint8_t irq);
    void (*unmask)(uint8_t irq);

    void (*start_timer)(uint32_t ms, uint8_t vector);

    int  (*alloc_vector)(void);
    void (*free_vector)(int vector);
};

extern struct interrupt_controller* g_intc;