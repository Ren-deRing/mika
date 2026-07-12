#include <kernel/blk_cache.h>
#include <kernel/ahci.h>
#include <kernel/lock.h>
#include <kernel/kmem.h>
#include <kernel/printf.h>
#include <kernel/init.h>

#include <string.h>

#define BLK_CACHE_SIZE 256

struct blk_cache_slot {
    int dev_id;
    uint64_t sector;
    uint8_t data[512];
    int valid;
    int dirty;
};

static struct blk_cache_slot blk_cache[BLK_CACHE_SIZE];
static int blk_cache_next;
static spinlock_t blk_cache_lock;

static int blk_cache_lookup(int dev_id, uint64_t sector) {
    for (int i = 0; i < BLK_CACHE_SIZE; i++) {
        if (blk_cache[i].valid &&
            blk_cache[i].dev_id == dev_id &&
            blk_cache[i].sector == sector)
            return i;
    }
    return -1;
}

static int blk_cache_evict(void) {
    int idx = blk_cache_next;
    blk_cache_next = (blk_cache_next + 1) % BLK_CACHE_SIZE;

    if (blk_cache[idx].dirty) {
        ahci_bwrite(blk_cache[idx].dev_id,
                    blk_cache[idx].sector,
                    blk_cache[idx].data);
        blk_cache[idx].dirty = 0;
    }
    blk_cache[idx].valid = 0;
    return idx;
}

int blk_cache_read(int dev_id, uint64_t sector, void *buf) {
    spin_lock(&blk_cache_lock);
    int idx = blk_cache_lookup(dev_id, sector);
    if (idx >= 0) {
        __builtin_memcpy(buf, blk_cache[idx].data, 512);
        spin_unlock(&blk_cache_lock);
        return 0;
    }
    spin_unlock(&blk_cache_lock);

    int ret = ahci_bread(dev_id, sector, buf);
    if (ret < 0) return ret;

    spin_lock(&blk_cache_lock);
    idx = blk_cache_evict();
    blk_cache[idx].dev_id = dev_id;
    blk_cache[idx].sector = sector;
    __builtin_memcpy(blk_cache[idx].data, buf, 512);
    blk_cache[idx].valid = 1;
    blk_cache[idx].dirty = 0;
    spin_unlock(&blk_cache_lock);
    return 0;
}

int blk_cache_write(int dev_id, uint64_t sector, const void *buf) {
    spin_lock(&blk_cache_lock);
    int idx = blk_cache_lookup(dev_id, sector);
    if (idx < 0) {
        idx = blk_cache_evict();
        blk_cache[idx].dev_id = dev_id;
        blk_cache[idx].sector = sector;
        blk_cache[idx].valid = 1;
    }
    __builtin_memcpy(blk_cache[idx].data, buf, 512);
    blk_cache[idx].dirty = 1;
    spin_unlock(&blk_cache_lock);
    return 0;
}

void blk_cache_flush(int dev_id) {
    spin_lock(&blk_cache_lock);
    for (int i = 0; i < BLK_CACHE_SIZE; i++) {
        if (blk_cache[i].valid && blk_cache[i].dirty &&
            (dev_id < 0 || blk_cache[i].dev_id == dev_id)) {
            ahci_bwrite(blk_cache[i].dev_id,
                        blk_cache[i].sector,
                        blk_cache[i].data);
            blk_cache[i].dirty = 0;
        }
    }
    spin_unlock(&blk_cache_lock);
}

void blk_cache_flush_all(void) {
    blk_cache_flush(-1);
}

static void blk_cache_init(void) {
    spin_lock_init(&blk_cache_lock);
    blk_cache_next = 0;
    for (int i = 0; i < BLK_CACHE_SIZE; i++)
        blk_cache[i].valid = 0;
}

dev_initcall(blk_cache_init);
