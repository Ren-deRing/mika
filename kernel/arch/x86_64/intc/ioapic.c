#include "acpi/acpi.h"

#include <kernel/mmu.h>
#include <kernel/printf.h>

#define IOAPIC_REG_INDEX  0x00
#define IOAPIC_REG_DATA   0x10

extern void outb(uint16_t port, uint8_t val);

volatile uint32_t* ioapic_base;

void ioapic_init() {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    mmu_map(mmu_get_kernel_map(), acpi_info.ioapic_addr, acpi_info.ioapic_paddr,
        MMU_FLAGS_READ | MMU_FLAGS_WRITE | MMU_FLAGS_NOCACHE);

    ioapic_base = (volatile uint32_t*)acpi_info.ioapic_addr;
}

void ioapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)((uintptr_t)ioapic_base + IOAPIC_REG_INDEX) = reg;
    *(volatile uint32_t*)((uintptr_t)ioapic_base + IOAPIC_REG_DATA) = val;
}

uint32_t ioapic_read(uint32_t reg) {
    *(volatile uint32_t*)((uintptr_t)ioapic_base + IOAPIC_REG_INDEX) = reg;
    return *(volatile uint32_t*)((uintptr_t)ioapic_base + IOAPIC_REG_DATA);
}

void ioapic_set_irq(uint8_t irq, uint64_t apic_id, uint8_t vector) {
    uint32_t low = vector;
    uint32_t high = (uint32_t)(apic_id << 24);

    ioapic_write(0x10 + irq * 2, low);
    ioapic_write(0x10 + irq * 2 + 1, high);
}

static uint8_t irq_to_gsi(uint8_t irq) {
    for (int i = 0; i < acpi_info.override_count; i++) {
        if (acpi_info.int_overrides[i].source == irq) {
            return acpi_info.int_overrides[i].target_gsi;
        }
    }
    return irq;
}

void x86_intc_map_irq(uint8_t irq, uint32_t target_cpu, uint8_t vector) {
    uint32_t lapic_id = acpi_info.cpu_ids[target_cpu];
    uint8_t gsi = irq_to_gsi(irq);
    ioapic_set_irq(gsi, lapic_id, vector);
}

void x86_intc_mask(uint8_t irq) {
    uint8_t gsi = irq_to_gsi(irq);
    uint32_t low = ioapic_read(0x10 + gsi * 2);
    ioapic_write(0x10 + gsi * 2, low | (1 << 16));
}

void x86_intc_unmask(uint8_t irq) {
    uint8_t gsi = irq_to_gsi(irq);
    uint32_t low = ioapic_read(0x10 + gsi * 2);
    ioapic_write(0x10 + gsi * 2, low & ~(1 << 16));
}