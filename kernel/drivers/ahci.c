#include <kernel/ahci.h>
#include <kernel/pci.h>
#include <kernel/mmu.h>
#include <kernel/printf.h>
#include <kernel/init.h>
#include <kernel/kmem.h>

extern void outl(uint16_t port, uint32_t val);
extern uint32_t inl(uint16_t port);

#define MAX_AHCI_DEVS 4
static struct ahci_dev g_ahci_devs[MAX_AHCI_DEVS];
static int g_ahci_count = 0;

static void ahci_port_write(volatile struct ahci_hba *hba, int port, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)((uintptr_t)&hba->ports[port] + reg) = val;
}

static uint32_t ahci_port_read(volatile struct ahci_hba *hba, int port, uint32_t reg) {
    return *(volatile uint32_t *)((uintptr_t)&hba->ports[port] + reg);
}

static int ahci_port_init(struct ahci_dev *dev, volatile struct ahci_hba *hba, int port) {
    // stop port DMA
    uint32_t cmd = ahci_port_read(hba, port, AHCI_PORT_CMD);
    if (cmd & AHCI_PXCMD_ST) {
        cmd &= ~AHCI_PXCMD_ST;
        ahci_port_write(hba, port, AHCI_PORT_CMD, cmd);
    }
    if (cmd & AHCI_PXCMD_FRE) {
        cmd &= ~AHCI_PXCMD_FRE;
        ahci_port_write(hba, port, AHCI_PORT_CMD, cmd);
    }

    // FR/CR clear 대기
    for (int i = 0; i < 10000; i++) {
        uint32_t c = ahci_port_read(hba, port, AHCI_PORT_CMD);
        if (!(c & (AHCI_PXCMD_FR | AHCI_PXCMD_CR))) break;
        outl(0x80, 0); // udelay ~1
    }

    // Allocate command list
    page_t *clb_page = page_alloc(0);
    dev->clb = p2v(page_to_phys(clb_page));
    dev->clb_phys = page_to_phys(clb_page);
    __builtin_memset(dev->clb, 0, PAGE_SIZE);

    // Allocate FIS base
    page_t *fb_page = page_alloc(0);
    dev->fb = p2v(page_to_phys(fb_page));
    dev->fb_phys = page_to_phys(fb_page);
    __builtin_memset(dev->fb, 0, PAGE_SIZE);

    // Allocate command tables
    page_t *ct_page = page_alloc(3); // order 3 = 32KB
    if (!ct_page) ct_page = page_alloc(2);
    if (!ct_page) ct_page = page_alloc(1);
    if (!ct_page) ct_page = page_alloc(0);
    if (!ct_page) {
        page_free(clb_page, 0);
        page_free(fb_page, 0);
        return -1;
    }
    dev->cmd_tables = p2v(page_to_phys(ct_page));
    dev->ct_phys = page_to_phys(ct_page);
    __builtin_memset(dev->cmd_tables, 0, PAGE_SIZE * (1 << 3));

    // Allocate single-sector DMA buffer
    page_t *buf_page = page_alloc(0);
    dev->buf = p2v(page_to_phys(buf_page));
    dev->buf_phys = page_to_phys(buf_page);

    // Set base addresses
    ahci_port_write(hba, port, AHCI_PORT_CLB,  (uint32_t)(dev->clb_phys & 0xFFFFFFFF));
    ahci_port_write(hba, port, AHCI_PORT_CLBU, (uint32_t)(dev->clb_phys >> 32));
    ahci_port_write(hba, port, AHCI_PORT_FB,   (uint32_t)(dev->fb_phys & 0xFFFFFFFF));
    ahci_port_write(hba, port, AHCI_PORT_FBU,  (uint32_t)(dev->fb_phys >> 32));

    // Clear SERR
    ahci_port_write(hba, port, AHCI_PORT_SERR, 0xFFFFFFFF);

    // Set PxCMD.SUD PxCMD.POD
    cmd = ahci_port_read(hba, port, AHCI_PORT_CMD);
    cmd |= AHCI_PXCMD_SUD | AHCI_PXCMD_POD;
    ahci_port_write(hba, port, AHCI_PORT_CMD, cmd);
    outl(0x80, 0);

    // Set PxCMD.ST PxCMD.FRE
    cmd = ahci_port_read(hba, port, AHCI_PORT_CMD);
    cmd |= AHCI_PXCMD_FRE | AHCI_PXCMD_ST;
    ahci_port_write(hba, port, AHCI_PORT_CMD, cmd);

    spin_lock_init(&dev->lock);
    return 0;
}

static int ahci_submit_cmd(struct ahci_dev *dev, volatile struct ahci_hba *hba,
                           uint8_t ata_cmd,
                           uint64_t lba, uint16_t count,
                           uint64_t data_phys, int write) {
    int slot = 0;

    for (int i = 0; i < 10000; i++) {
        uint32_t ci = ahci_port_read(hba, dev->port_idx, AHCI_PORT_CI);
        uint32_t sact = ahci_port_read(hba, dev->port_idx, AHCI_PORT_SACT);
        if (!(ci & (1 << slot)) && !(sact & (1 << slot))) break;
        slot = (slot + 1) % 32;
    }

    uint32_t ci = ahci_port_read(hba, dev->port_idx, AHCI_PORT_CI);
    if (ci & (1 << slot)) {
        dprintf("[AHCI] No free command slot!\n");
        return -1;
    }

    // Build command header
    struct ahci_cmd_header *header = (struct ahci_cmd_header *)dev->clb + slot;
    __builtin_memset(header, 0, sizeof(*header));

    uint32_t ct_offset = slot * sizeof(struct ahci_cmd_table);
    header->ctba  = (uint32_t)((dev->ct_phys + ct_offset) & 0xFFFFFFFF);
    header->ctbau = (uint32_t)((dev->ct_phys + ct_offset) >> 32);
    header->flags = (1 << 15) | (5 << 0);  // C=1, Cfl=5 (2 dwords CFIS)
    if (write) header->flags |= (1 << 6);  // W=1

    // Build command table
    struct ahci_cmd_table *cmdtab = (struct ahci_cmd_table *)((uint8_t *)dev->cmd_tables + ct_offset);
    __builtin_memset(cmdtab, 0, sizeof(struct ahci_cmd_table));

    // Register H2D FIS
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmdtab->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ata_cmd;
    fis->device = 0x40; // LBA mode
    fis->lba0 = (uint8_t)(lba >> 0);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->count_low  = (uint8_t)(count & 0xFF);
    fis->count_high = (uint8_t)(count >> 8);

    // PRDT
    cmdtab->prdt[0].dba  = (uint32_t)(data_phys & 0xFFFFFFFF);
    cmdtab->prdt[0].dbau = (uint32_t)(data_phys >> 32);
    cmdtab->prdt[0].dbc  = (count * SECTOR_SIZE - 1) | AHCI_PRDT_IOC;
    header->prdtl = 1;

    // Issue command
    __sync_synchronize();
    ahci_port_write(hba, dev->port_idx, AHCI_PORT_CI, 1 << slot);

    // Wait for completion
    for (int i = 0; i < 1000000; i++) {
        ci = ahci_port_read(hba, dev->port_idx, AHCI_PORT_CI);
        if (!(ci & (1 << slot))) {
            // Check error
            uint32_t is = ahci_port_read(hba, dev->port_idx, AHCI_PORT_IS);
            if (is & (1 << 30)) { // PxIS.TFES
                dprintf("[AHCI] Task file error on slot %d\n", slot);
                ahci_port_write(hba, dev->port_idx, AHCI_PORT_IS, is);
                return -1;
            }
            ahci_port_write(hba, dev->port_idx, AHCI_PORT_IS, is);
            return 0;
        }
        outl(0x80, 0); // yield
    }

    dprintf("[AHCI] Command timeout on slot %d\n", slot);
    return -1;
}

int ahci_read(struct ahci_dev *dev, uint64_t sector, void *buf) {
    int ret = ahci_submit_cmd(dev, dev->hba,
                              ATA_CMD_READ_DMA_EXT, sector, 1,
                              dev->buf_phys, 0);
    if (ret == 0) {
        __builtin_memcpy(buf, dev->buf, SECTOR_SIZE);
    }
    return ret;
}

int ahci_write(struct ahci_dev *dev, uint64_t sector, const void *buf) {
    __builtin_memcpy(dev->buf, buf, SECTOR_SIZE);
    return ahci_submit_cmd(dev, dev->hba,
                           ATA_CMD_WRITE_DMA_EXT, sector, 1,
                           dev->buf_phys, 1);
}

int ahci_probe(volatile struct ahci_hba *hba, int port_idx) {
    uint32_t ssts = *(volatile uint32_t *)((uintptr_t)&hba->ports[port_idx] + AHCI_PORT_SSTS);
    int det = ssts & 0x0F;

    if (det == AHCI_DET_PRESENT) {
        // present but not established
        volatile uint32_t *sctl_reg = (volatile uint32_t *)((uintptr_t)&hba->ports[port_idx] + AHCI_PORT_SCTL);
        uint32_t sctl = *sctl_reg;
        sctl = (sctl & ~0x0F) | 1;
        *sctl_reg = sctl;
        outl(0x80, 0); outl(0x80, 0);
        sctl = (sctl & ~0x0F) | 0;
        *sctl_reg = sctl;

        for (int i = 0; i < 10000; i++) {
            ssts = *(volatile uint32_t *)((uintptr_t)&hba->ports[port_idx] + AHCI_PORT_SSTS);
            det = ssts & 0x0F;
            if (det == AHCI_DET_ESTABLISHED) break;
            if (det != AHCI_DET_ESTABLISHED && det != AHCI_DET_PRESENT) break; // gone
        }
    }
    // DET=0 or DET=3 — nothing to do
    // DET=1 after COMRESET still NOT established — skip

    det = ssts & 0x0F;
    if (det != AHCI_DET_PRESENT && det != AHCI_DET_ESTABLISHED) {
        return -1;
    }

    uint32_t sig = *(volatile uint32_t *)((uintptr_t)&hba->ports[port_idx] + AHCI_PORT_SIG);
    if (sig != 0x00000101 && sig != 0x00000000) {
        return -1;
    }

    if (g_ahci_count >= MAX_AHCI_DEVS) return -1;
    struct ahci_dev *dev = &g_ahci_devs[g_ahci_count];
    __builtin_memset(dev, 0, sizeof(*dev));
    dev->hba = hba;
    dev->port_idx = port_idx;

    if (ahci_port_init(dev, hba, port_idx) < 0) return -1;

    // Issue IDENTIFY
    if (ahci_submit_cmd(dev, hba, ATA_CMD_IDENTIFY, 0, 1, dev->buf_phys, 0) == 0) {
        uint16_t *ident = (uint16_t *)dev->buf;
        uint64_t lba28 = ident[60] | ((uint64_t)ident[61] << 16);
        uint64_t lba48 = ident[100] | ((uint64_t)ident[101] << 16)
                       | ((uint64_t)ident[102] << 32) | ((uint64_t)ident[103] << 48);
        dev->sector_count = lba48 ? lba48 : lba28;
    } else {
        dprintf("[AHCI] ATA device on port %d (dev=%d) IDENTIFY failed, defaulting to 64MB\n",
                port_idx, g_ahci_count);
        dev->sector_count = (64 * 1024 * 1024) / SECTOR_SIZE;
    }
    g_ahci_count++;
    return g_ahci_count - 1;
}

static struct ahci_dev *ahci_find_dev(int dev_id) {
    if (dev_id < 0 || dev_id >= g_ahci_count) return NULL;
    return &g_ahci_devs[dev_id];
}

// Block device API (locked)
int64_t ahci_bread(int dev_id, uint64_t sector, void *buf) {
    struct ahci_dev *dev = ahci_find_dev(dev_id);
    if (!dev) return -1;
    spin_lock(&dev->lock);
    int ret = ahci_read(dev, sector, buf);
    spin_unlock(&dev->lock);
    return ret;
}

int64_t ahci_bwrite(int dev_id, uint64_t sector, const void *buf) {
    struct ahci_dev *dev = ahci_find_dev(dev_id);
    if (!dev) return -1;
    spin_lock(&dev->lock);
    int ret = ahci_write(dev, sector, buf);
    spin_unlock(&dev->lock);
    return ret;
}

static int ahci_pci_callback(struct pci_dev *pdev, void *ctx) {
    (void)ctx;

    if (pdev->class_code != AHCI_PCI_CLASS ||
        pdev->subclass != AHCI_PCI_SUBCLASS) {
        return 0; // not AHCI
    }

    int abar_idx = 5;
    uint64_t abar_phys = pdev->bar[abar_idx];
    abar_phys &= ~0xF;
    // 64bits 체크
    if (!pdev->bar_is_io[abar_idx] && (pdev->bar[abar_idx] & 0x06) == 0x04 && abar_idx < 5) {
        abar_phys = pdev->bar[abar_idx] | ((uint64_t)pdev->bar[abar_idx + 1] << 32);
    }

    uintptr_t abar_virt = (uintptr_t)p2v(abar_phys);
    mmu_map(mmu_get_kernel_map(), abar_virt, abar_phys,
            MMU_FLAGS_READ | MMU_FLAGS_WRITE | MMU_FLAGS_NOCACHE);

    volatile struct ahci_hba *hba = (volatile struct ahci_hba *)abar_virt;

    uint32_t cap = hba->cap;
    uint32_t pi = hba->pi;
    int nports = AHCI_CAP_NP(cap) + 1;

    // AHCI mode 활성화
    hba->ghc |= AHCI_GHC_AE;

    int found = 0;

    for (int p = 0; p < 32 && p < nports; p++) {
        if (!(pi & (1 << p))) continue;
        uint32_t cmd = ahci_port_read(hba, p, AHCI_PORT_CMD);
        cmd |= AHCI_PXCMD_SUD | AHCI_PXCMD_POD;
        ahci_port_write(hba, p, AHCI_PORT_CMD, cmd);
    }
    outl(0x80, 0); outl(0x80, 0);

    for (int p = 0; p < 32 && p < nports; p++) {
        if (!(pi & (1 << p))) continue;
        if (ahci_probe(hba, p) >= 0) found++;
    }

    return 0;
}

static void ahci_init(void) {
    pci_scan(ahci_pci_callback, NULL);
}

dev_initcall(ahci_init, PRIO_THIRD);

#include <kernel/blockdev.h>

static void ahci_register_bdevs(void) {
    for (int i = 0; i < g_ahci_count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "sd%c", 'a' + i);
        blockdev_register(name, i, g_ahci_devs[i].sector_count);
    }
}

subsys_initcall(ahci_register_bdevs, PRIO_FOURTH);
