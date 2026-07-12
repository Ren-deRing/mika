#include <kernel/intc.h>
#include <kernel/printf.h>
#include <kernel/init.h>
#include <kernel/mmu.h>
#include <kernel/clock.h>

#include "acpi/acpi.h"

struct interrupt_controller* g_intc = NULL;

#define LAPIC_ID            0x0020
#define LAPIC_VER           0x0030
#define LAPIC_TPR           0x0080
#define LAPIC_EOI           0x00B0
#define LAPIC_SIVR          0x00F0
#define LAPIC_ICR_LOW       0x0300
#define LAPIC_ICR_HIGH      0x0310
#define LAPIC_LVT_TIMER     0x0320
#define LAPIC_LVT_ERROR     0x0370

#define LAPIC_TICR          0x0380 
#define LAPIC_TCCR          0x0390
#define LAPIC_TDCR          0x03E0

static uint32_t vector_bitmap[8];

static int lapic_alloc_vector(void) {
    for (int vec = 48; vec < 256; vec++) {
        int idx = vec / 32;
        int bit = vec % 32;
        if (!(vector_bitmap[idx] & (1u << bit))) {
            vector_bitmap[idx] |= (1u << bit);
            return vec;
        }
    }
    return -1;
}

static void lapic_free_vector(int vector) {
    if (vector < 0 || vector >= 256) return;
    int idx = vector / 32;
    int bit = vector % 32;
    vector_bitmap[idx] &= ~(1u << bit);
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(acpi_info.lapic_addr + reg) = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t*)(acpi_info.lapic_addr + reg);
}

void x86_lapic_set_periodic(uint32_t ms, uint8_t vector) {
    struct cpu* core = curcpu;
    if (!core || !core->timer_ready) return;

    lapic_write(LAPIC_TDCR, 0x03);
    lapic_write(LAPIC_LVT_TIMER, vector | (1 << 17));

    lapic_write(LAPIC_TICR, core->timer_ticks_per_ms * ms);

    udelay(1);
    if (lapic_read(LAPIC_TCCR) == lapic_read(LAPIC_TICR)) {
        dprintf("Error: LAPIC Timer is NOT counting!\n");
    }
}

// Calibrate
void x86_lapic_calibrate_timer(void) {
    if (!curcpu) return;

    lapic_write(LAPIC_TDCR, 0x03); 

    lapic_write(LAPIC_TICR, 0xFFFFFFFF);

    udelay(10000);

    uint32_t current_tick = lapic_read(LAPIC_TCCR);
    uint32_t ticks_in_10ms = 0xFFFFFFFF - current_tick;

    curcpu->timer_ticks_per_ms = ticks_in_10ms / 10;
    curcpu->timer_ready = true;
}

/* IPI */
void x86_lapic_send_ipi(uint32_t target_lapic_id, uint8_t vector) {
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12)) {
        arch_pause();
    }

    lapic_write(LAPIC_ICR_HIGH, target_lapic_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector); 
}

/* EOI */
void x86_lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

int x86_get_cpu_count(void) {
    return acpi_info.cpu_count;
}

void x86_lapic_init_local(void) {
    if (!acpi_info.lapic_addr) return;

    lapic_write(LAPIC_TPR, 0);

    lapic_write(LAPIC_SIVR, 0xFF | (1 << 8));
    lapic_write(LAPIC_LVT_ERROR, 1 << 16);

    x86_lapic_calibrate_timer();
}

extern void register_handler(uint8_t vector, handler_t handler, void *data);

// ioapic
extern void ioapic_init(void);
extern void x86_intc_map_irq(uint8_t irq, uint32_t target_cpu, uint8_t vector);
extern void x86_intc_mask(uint8_t irq);
extern void x86_intc_unmask(uint8_t irq);

void x86_intc_register(uint8_t vector, handler_t handler, void *data) {
    register_handler(vector, handler, data);
}

uint32_t x86_get_cpu_id(int index) {
    if (index < 0 || index >= acpi_info.cpu_count) return 0xFFFFFFFF;
    return acpi_info.cpu_ids[index];
}

uint32_t x86_get_hw_id(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= acpi_info.cpu_count) return 0xFFFFFFFF;
    return acpi_info.cpu_ids[cpu_index];
}

static struct interrupt_controller x86_intc = {
    .name = "x86_LAPIC",
    .get_cpu_count = x86_get_cpu_count,
    .get_cpu_id = x86_get_cpu_id,
    .get_hw_id = x86_get_hw_id,
    .init_local = x86_lapic_init_local,
    .register_handler = x86_intc_register,
    .send_ipi = x86_lapic_send_ipi,
    .eoi = x86_lapic_eoi,
    .map_irq = x86_intc_map_irq, 
    .mask = x86_intc_mask,
    .unmask = x86_intc_unmask,
    .start_timer = x86_lapic_set_periodic,
    .alloc_vector = lapic_alloc_vector,
    .free_vector = lapic_free_vector,
};

void lapic_controller_init(void) {
    g_intc = &x86_intc;

    for (int i = 0; i < 32; i++) {
        int idx = i / 32; int bit = i % 32;
        vector_bitmap[idx] |= (1u << bit);
    }
    vector_bitmap[0x21 / 32] |= (1u << (0x21 % 32));
    vector_bitmap[0x40 / 32] |= (1u << (0x40 % 32));

    mmu_map(mmu_get_kernel_map(), acpi_info.lapic_addr, acpi_info.lapic_paddr,
        MMU_FLAGS_READ | MMU_FLAGS_WRITE | MMU_FLAGS_NOCACHE);

    ioapic_init();
    
    if (g_intc->init_local) {
        g_intc->init_local();
    }
}

void ap_lapic_init(void) {
    if (g_intc && g_intc->init_local) {
        g_intc->init_local();
    }
    if (g_intc && g_intc->start_timer) {
        g_intc->start_timer(1, 0x40);
    }
}

dev_initcall(lapic_controller_init, PRIO_SECOND);
ap_arch_initcall(ap_lapic_init, PRIO_THIRD);