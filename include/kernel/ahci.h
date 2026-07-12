#pragma once

#include <stdint.h>
#include <kernel/lock.h>

#define AHCI_PCI_CLASS    0x01
#define AHCI_PCI_SUBCLASS 0x06
#define AHCI_PCI_PROGIF   0x01

// HBA
struct ahci_hba {
    uint32_t cap;        // 0x00 — Host Capabilities
    uint32_t ghc;        // 0x04 — Global Host Control
    uint32_t is;         // 0x08 — Interrupt Status
    uint32_t pi;         // 0x0C — Ports Implemented
    uint32_t vs;         // 0x10 — Version
    uint32_t ccc_ctl;    // 0x14 — Command Completion Coalescing Control
    uint32_t ccc_ports;  // 0x18 — Command Completion Coalescing Ports
    uint32_t em_loc;     // 0x1C — Enclosure Management Location
    uint32_t em_ctl;     // 0x20 — Enclosure Management Control
    uint32_t cap2;       // 0x24 — Host Capabilities Extended
    uint32_t bohc;       // 0x28 — BIOS/OS Handoff Control
    uint8_t  res[0x100 - 0x2C];
    uint8_t  ports[32][0x80]; // 0x100 — Port registers
} __attribute__((packed));

// CAP
#define AHCI_CAP_NP(c)          (((c) >> 0) & 0x1F)
#define AHCI_CAP_SXS            (1 << 5)
#define AHCI_CAP_EMS            (1 << 6)
#define AHCI_CAP_CCC            (1 << 7)
#define AHCI_CAP_NCS(c)         (((c) >> 8) & 0x1F)
#define AHCI_CAP_PSC            (1 << 13)
#define AHCI_CAP_SSC            (1 << 14)
#define AHCI_CAP_PMD            (1 << 15)
#define AHCI_CAP_FBSS           (1 << 16)
#define AHCI_CAP_SPM            (1 << 17)
#define AHCI_CAP_SAM            (1 << 18)
#define AHCI_CAP_ISS(c)         (((c) >> 20) & 0x0F)
#define AHCI_CAP_SCLO           (1 << 24)
#define AHCI_CAP_SAL            (1 << 25)
#define AHCI_CAP_SALP           (1 << 26)
#define AHCI_CAP_SSS            (1 << 27)
#define AHCI_CAP_SMPS           (1 << 28)
#define AHCI_CAP_SSNTF          (1 << 29)
#define AHCI_CAP_SNCQ           (1 << 30)
#define AHCI_CAP_S64A           (1u << 31)

// GHC
#define AHCI_GHC_HR            (1 << 0)
#define AHCI_GHC_IE            (1 << 1)
#define AHCI_GHC_MRSM          (1 << 2)
#define AHCI_GHC_AE            (1 << 31)

// PxCMD
#define AHCI_PXCMD_ST          (1 << 0)
#define AHCI_PXCMD_SUD         (1 << 1)
#define AHCI_PXCMD_POD         (1 << 2)
#define AHCI_PXCMD_CLO         (1 << 3)
#define AHCI_PXCMD_FRE         (1 << 4)
#define AHCI_PXCMD_CCS(c)      (((c) >> 8) & 0x1F)
#define AHCI_PXCMD_MPSS        (1 << 13)
#define AHCI_PXCMD_FR          (1 << 14)
#define AHCI_PXCMD_CR          (1 << 15)
#define AHCI_PXCMD_CPS         (1 << 16)
#define AHCI_PXCMD_PMA         (1 << 17)
#define AHCI_PXCMD_HPCP        (1 << 18)
#define AHCI_PXCMD_MPSP        (1 << 19)
#define AHCI_PXCMD_CPD         (1 << 20)
#define AHCI_PXCMD_ESP         (1 << 21)
#define AHCI_PXCMD_FBSCP       (1 << 22)
#define AHCI_PXCMD_APSTE       (1 << 23)
#define AHCI_PXCMD_ATAPI       (1 << 24)
#define AHCI_PXCMD_DLAE        (1 << 25)
#define AHCI_PXCMD_ALPE        (1 << 26)
#define AHCI_PXCMD_ASP         (1 << 27)
#define AHCI_PXCMD_ICC(c)      (((c) >> 28) & 0x0F)

// PxSSTS
#define AHCI_PXSSTS_DET(c)     ((c) & 0x0F)
#define   AHCI_DET_NODEVICE    0
#define   AHCI_DET_PRESENT     1
#define   AHCI_DET_ESTABLISHED 3

#define AHCI_PORT_RES         0x80
#define AHCI_PORT_CLB         0x00
#define AHCI_PORT_CLBU        0x04
#define AHCI_PORT_FB          0x08
#define AHCI_PORT_FBU         0x0C
#define AHCI_PORT_IS          0x10
#define AHCI_PORT_IE          0x14
#define AHCI_PORT_CMD         0x18
#define AHCI_PORT_TFD         0x20
#define AHCI_PORT_SIG         0x24
#define AHCI_PORT_SSTS        0x28
#define AHCI_PORT_SCTL        0x2C
#define AHCI_PORT_SERR        0x30
#define AHCI_PORT_SACT        0x34
#define AHCI_PORT_CI          0x38
#define AHCI_PORT_SNTF        0x3C
#define AHCI_PORT_FBS         0x40

// Command header
struct ahci_cmd_header {
    uint16_t flags;
    uint16_t prdtl;     // Physical Region Descriptor Table Length
    uint32_t prdbc;     // PRD Byte Count
    uint32_t ctba;      // Command Table Descriptor Base Address (lower)
    uint32_t ctbau;     // Command Table Descriptor Base Address (upper)
    uint32_t rsvd[4];
} __attribute__((packed));

#define AHCI_CMD_FLAG_C        (1 << 15)  // Clear Busy upon R_OK
#define AHCI_CMD_FLAG_B        (1 << 14)  // BIST
#define AHCI_CMD_FLAG_R        (1 << 13)  // Reset
#define AHCI_CMD_FLAG_P        (1 << 12)  // Prefetchable
#define AHCI_CMD_FLAG_W        (1 << 6)   // Write
#define AHCI_CMD_FLAG_A        (1 << 5)   // ATAPI
#define AHCI_CMD_FLAG_T        (1 << 4)   // T8
#define AHCI_CMD_FLAG_D        (1 << 2)   // Table
#define AHCI_CMD_FLAG_Cfl(c)   ((c) & 0x1F)

// PRDT entry
struct ahci_prdt {
    uint32_t dba;       // Data Base Address (lower)
    uint32_t dbau;      // Data Base Address (upper)
    uint32_t rsvd;
    uint32_t dbc;       // Byte count
} __attribute__((packed));

#define AHCI_PRDT_DBC(c)      ((c) & 0x3FFFFF)
#define AHCI_PRDT_IOC          (1 << 31)

// Command table
struct ahci_cmd_table {
    uint8_t  cfis[64];  // Command FIS
    uint8_t  acmd[16];  // ATAPI command
    uint8_t  rsvd[48];
    struct ahci_prdt prdt[];
} __attribute__((packed));

// FIS
#define FIS_TYPE_REG_H2D      0x27
#define FIS_TYPE_REG_D2H      0x34
#define FIS_TYPE_DMA_ACT      0x39
#define FIS_TYPE_DMA_SETUP    0x41
#define FIS_TYPE_DATA         0x46
#define FIS_TYPE_BIST         0x58
#define FIS_TYPE_PIO_SETUP    0x5F
#define FIS_TYPE_DEV_BITS     0xA1

// Register FIS
struct fis_reg_h2d {
    uint8_t  fis_type;    // 0x27
    uint8_t  pmport:4;
    uint8_t  rsvd0:3;
    uint8_t  c:1;         // 1 = Command
    uint8_t  command;
    uint8_t  feature_low;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  feature_high;
    uint8_t  count_low;
    uint8_t  count_high;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsvd1[4];
} __attribute__((packed));

// ATA
#define ATA_CMD_READ_DMA      0xC8
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA     0xCA
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_FLUSH_CACHE   0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

#define SECTOR_SIZE 512

struct ahci_dev {
    volatile struct ahci_hba *hba;
    int   port_idx;
    int   pci_vec;       // MSI vector
    int   cmd_slot_count;

    // DMA buffers
    void  *clb;          // Command list base (1KB)
    void  *fb;           // FIS base (256 bytes)
    void  *cmd_tables;   // Command tables (4KB)
    uint64_t clb_phys;
    uint64_t fb_phys;
    uint64_t ct_phys;

    uint8_t  *buf;
    uint64_t  buf_phys;

    uint64_t sector_count;
    spinlock_t lock;
};

int ahci_probe(volatile struct ahci_hba *hba, int port_idx);
int ahci_read(struct ahci_dev *dev, uint64_t sector, void *buf);
int ahci_write(struct ahci_dev *dev, uint64_t sector, const void *buf);
int64_t ahci_bread(int dev_id, uint64_t sector, void *buf);
int64_t ahci_bwrite(int dev_id, uint64_t sector, const void *buf);
