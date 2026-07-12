#pragma once

#include <stdint.h>

struct vnode;

struct blkdev {
    int      dev_id;
    uint64_t sector_count;
};

int blockdev_register(const char *name, int dev_id, uint64_t sector_count);
int blockdev_get_devid(struct vnode *vp);
