#include <kernel/pci.h>
#include <kernel/printf.h>
#include <kernel/init.h>
#include <kernel/mmu.h>
#include <kernel/msi.h>

extern void outl(uint16_t port, uint32_t val);
extern uint32_t inl(uint16_t port);

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000 | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = 0x80000000 | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset & ~2);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t orig = pci_config_read32(bus, slot, func, offset & ~2);
    uint32_t mask = 0xFFFF << ((offset & 2) * 8);
    uint32_t new = (orig & ~mask) | (((uint32_t)val) << ((offset & 2) * 8));
    pci_config_write32(bus, slot, func, offset & ~2, new);
}

static uint8_t pci_read_cap_pointer(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t status = pci_config_read16(bus, slot, func, 0x06);
    if (!(status & (1 << 4))) return 0;
    return pci_config_read16(bus, slot, func, 0x34) & 0xFF;
}

static int pci_parse_msix(struct pci_dev *dev) {
    uint8_t cap = pci_read_cap_pointer(dev->bus, dev->slot, dev->func);
    while (cap) {
        uint8_t id = pci_config_read16(dev->bus, dev->slot, dev->func, cap) & 0xFF;
        if (id == PCI_CAP_ID_MSIX) {
            struct msix_capability msix;
            uint32_t raw[3];
            raw[0] = pci_config_read32(dev->bus, dev->slot, dev->func, cap);
            raw[1] = pci_config_read32(dev->bus, dev->slot, dev->func, cap + 4);
            raw[2] = pci_config_read32(dev->bus, dev->slot, dev->func, cap + 8);
            __builtin_memcpy(&msix, raw, sizeof(msix));

            dev->msix_cap_offset = cap;
            dev->msix_table_bir = MSIX_TABLE_BIR(msix.table_offset);
            dev->msix_table_offset = MSIX_TABLE_OFFSET(msix.table_offset);
            dev->msix_table_entries = MSIX_CTRL_TABLE_SIZE(msix.msg_ctrl);
            dev->msix_pba_bir = MSIX_TABLE_BIR(msix.pba_offset);
            dev->msix_pba_offset = MSIX_TABLE_OFFSET(msix.pba_offset);
            return 0;
        }
        cap = pci_config_read16(dev->bus, dev->slot, dev->func, cap + 1) >> 8;
    }
    return -1;
}

static void pci_read_bar(struct pci_dev *dev, int idx) {
    uint8_t offset = 0x10 + idx * 4;
    uint32_t low = pci_config_read32(dev->bus, dev->slot, dev->func, offset);

    if (low & 1) {
        dev->bar[idx] = low & ~0x3;
        dev->bar_is_io[idx] = true;
    } else {
        dev->bar_is_io[idx] = false;
        if ((low & 0x06) == 0x04) {
            uint32_t high = pci_config_read32(dev->bus, dev->slot, dev->func, offset + 4);
            dev->bar[idx] = low | ((uint64_t)high << 32);
        } else {
            dev->bar[idx] = low & ~0xF;
        }
    }
}

static int pci_scan_bus(int (*cb)(struct pci_dev *dev, void *ctx), void *ctx) {
    int count = 0;
    for (int slot = 0; slot < 32; slot++) {
        uint16_t vendor = pci_config_read16(0, slot, 0, 0x00);
        if (vendor == 0xFFFF || vendor == 0x0000) continue;

        uint16_t reg_0e = pci_config_read16(0, slot, 0, 0x0E);
        uint8_t hdr_type = reg_0e & 0xFF;
        int max_func = (hdr_type & 0x80) ? 8 : 1;

        for (int func = 0; func < max_func; func++) {
            vendor = pci_config_read16(0, slot, func, 0x00);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            struct pci_dev dev;
            __builtin_memset(&dev, 0, sizeof(dev));
            dev.bus = 0;
            dev.slot = slot;
            dev.func = func;
            dev.vendor_id = vendor;
            dev.device_id = pci_config_read16(0, slot, func, 0x02);
            dev.rev_id = pci_config_read16(0, slot, func, 0x08) & 0xFF;
            dev.prog_if = (pci_config_read16(0, slot, func, 0x08) >> 8) & 0xFF;
            dev.subclass = pci_config_read16(0, slot, func, 0x0A) & 0xFF;
            dev.class_code = (pci_config_read16(0, slot, func, 0x0A) >> 8) & 0xFF;
            dev.irq_pin = (pci_config_read16(0, slot, func, 0x3C) >> 8) & 0xFF;
            dev.irq_line = pci_config_read16(0, slot, func, 0x3C) & 0xFF;
            dev.msix_cap_offset = 0;
            dev.msix_table_bir = -1;

            for (int b = 0; b < 6; b++) pci_read_bar(&dev, b);

            if (cb && cb(&dev, ctx)) return count + 1;
            count++;
        }
    }
    return count;
}

int pci_scan(int (*cb)(struct pci_dev *dev, void *ctx), void *ctx) {
    return pci_scan_bus(cb, ctx);
}

static int pci_init_cb(struct pci_dev *dev, void *ctx) {
    (void)ctx;
    (void)dev;
    return 0;
}
