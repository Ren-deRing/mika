#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PCI_VENDOR_VIRTIO    0x1AF4
#define PCI_DEVICE_VIRTIO_BLK 0x1001

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA      0x06

#define PCI_CAP_ID_MSIX 0x11

#define PCI_BAR_MEM     0x00
#define PCI_BAR_IO      0x01

struct pci_dev {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  rev_id;
    uint8_t  irq_pin;
    uint8_t  irq_line;

    uint64_t bar[6];
    bool     bar_is_io[6];

    uint8_t  msix_cap_offset;
    int      msix_table_bir;
    uint32_t msix_table_offset;
    int      msix_table_entries;
    int      msix_pba_bir;
    uint32_t msix_pba_offset;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);

int pci_scan(int (*cb)(struct pci_dev *dev, void *ctx), void *ctx);
