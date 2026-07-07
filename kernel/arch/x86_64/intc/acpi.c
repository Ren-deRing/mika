#include <limine.h>

#include <kernel/mmu.h>
#include <kernel/intc.h>
#include <kernel/init.h>
#include <kernel/printf.h>

#include <boot/bootinfo.h>

#include "acpi/acpi.h"

#include <string.h>

struct acpi_info acpi_info = {0};

// TODO: REVISION PANIC

void acpi_init() {
    struct rsdp_descriptor* rsdp = (struct rsdp_descriptor*)g_boot_info.rsdp_address;

    if (rsdp->revision == 0) {
        dprintf("ACPI: REVISION IS 0! (NOT SUPPORTED)\n"); // TODO: RSDP
        dprintf("ACPI: PANIC, HALTING\n");
        for (;;) arch_halt();
        return;
    }

    struct xsdt* xsdt = (struct xsdt*)(phys_to_virt(rsdp->xsdt_address));
    int entries = (xsdt->header.length - sizeof(struct acpi_sdt_header)) / sizeof(uint64_t);

    for (int i = 0; i < entries; i++) {
        struct acpi_sdt_header* table = (struct acpi_sdt_header*)(phys_to_virt(xsdt->tables[i]));

        // MADT
        if (memcmp(table->signature, "APIC", 4) == 0) {
            acpi_info.madt = (struct madt*)table;
            madt_init(acpi_info.madt);
        }
        // FADT
        else if (memcmp(table->signature, "FACP", 4) == 0) {
            acpi_info.fadt = (struct fadt*)table;
        }
        // HPET
        else if (memcmp(table->signature, "HPET", 4) == 0) {
            struct hpet* h = (struct hpet*)table;
    
            uint64_t val = h->address.address;
            acpi_info.hpet_paddr = (uintptr_t)val;
            acpi_info.hpet_addr = (uintptr_t)(phys_to_virt(val));
        }
    }
}

void madt_init(struct madt* m) {
    acpi_info.lapic_paddr = m->lapic_addr;
    acpi_info.lapic_addr = (uintptr_t)phys_to_virt(m->lapic_addr);

    uint8_t* ptr = m->entries;
    uint8_t* end = (uint8_t*)m + m->header.length;

    while (ptr < end) {
        struct madt_entry_header* entry = (struct madt_entry_header*)ptr;
        if (entry->length < 2) break;

        switch (entry->type) {
            case 0: { // Processor Local APIC
                uint32_t flags = *(uint32_t*)&ptr[4];
                if ((flags & 1) && acpi_info.cpu_count < MAX_CPUS) {
                    acpi_info.cpu_ids[acpi_info.cpu_count++] = ptr[3]; // APIC ID
                }
                break;
            }
            case 1: { // I/O APIC
                acpi_info.ioapic_paddr = (uintptr_t)(*(uint32_t*)&ptr[4]);
                acpi_info.ioapic_addr = (uintptr_t)(phys_to_virt(*(uint32_t*)&ptr[4]));
                break;
            }
            case 2: { // Interrupt Source Override
                if (acpi_info.override_count < MAX_ISO) {
                    acpi_info.int_overrides[acpi_info.override_count].source = ptr[3];
                    acpi_info.int_overrides[acpi_info.override_count].target_gsi = *(uint32_t*)&ptr[4];
                    acpi_info.override_count++;
                }
                break;
            }
        }
        ptr += entry->length;
    }
}

arch_initcall(acpi_init, PRIO_THIRD);