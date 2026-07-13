#include <kernel/blockdev.h>
#include <kernel/fs/vnode.h>
#include <kernel/fs/vfs.h>
#include <kernel/ahci.h>
#include <kernel/blk_cache.h>
#include <kernel/kmem.h>
#include <kernel/printf.h>

#include <uapi/errno.h>
#include <uapi/sys/stat.h>

#include <string.h>

static struct vnode_ops blockdev_ops;

static ssize_t blockdev_read(struct vnode *vp, void *buf, size_t count, off_t off) {
    struct blkdev *blk = (struct blkdev *)vp->data;
    if (!blk) return -ENXIO;
    if (count == 0) return 0;

    uint64_t sector_start = (uint64_t)off / 512;
    uint64_t sector_end = ((uint64_t)off + count + 511) / 512;
    size_t done = 0;
    uint8_t tmp[512] __attribute__((aligned(16)));

    for (uint64_t s = sector_start; s < sector_end && s < blk->sector_count; s++) {
        if (ahci_bread(blk->dev_id, s, tmp) < 0)
            return done ? done : -EIO;

        size_t chunk_off = (s == sector_start) ? ((size_t)off % 512) : 0;
        size_t chunk_len = 512 - chunk_off;
        if (chunk_len > count - done)
            chunk_len = count - done;

        __builtin_memcpy((uint8_t *)buf + done, tmp + chunk_off, chunk_len);
        done += chunk_len;
    }
    return done;
}

static ssize_t blockdev_write(struct vnode *vp, const void *buf, size_t count, off_t off) {
    struct blkdev *blk = (struct blkdev *)vp->data;
    if (!blk) return -ENXIO;
    if (count == 0) return 0;

    uint64_t sector_start = (uint64_t)off / 512;
    uint64_t sector_end = ((uint64_t)off + count + 511) / 512;
    size_t done = 0;
    uint8_t tmp[512] __attribute__((aligned(16)));

    for (uint64_t s = sector_start; s < sector_end && s < blk->sector_count; s++) {
        size_t chunk_off = (s == sector_start) ? ((size_t)off % 512) : 0;
        size_t chunk_len = 512 - chunk_off;
        if (chunk_len > count - done)
            chunk_len = count - done;

        if (chunk_off == 0 && chunk_len == 512) {
            if (ahci_bwrite(blk->dev_id, s, (uint8_t *)buf + done) < 0)
                return done ? done : -EIO;
            blk_cache_invalidate(blk->dev_id, s);
        } else {
            if (ahci_bread(blk->dev_id, s, tmp) < 0)
                return done ? done : -EIO;
            __builtin_memcpy(tmp + chunk_off, (uint8_t *)buf + done, chunk_len);
            if (ahci_bwrite(blk->dev_id, s, tmp) < 0)
                return done ? done : -EIO;
            blk_cache_invalidate(blk->dev_id, s);
        }

        done += chunk_len;
    }
    return done;
}

static int blockdev_getattr(struct vnode *vp, struct stat *st) {
    struct blkdev *blk = (struct blkdev *)vp->data;
    if (!blk) return -ENXIO;

    st->st_mode  = S_IFBLK | 0660;
    st->st_rdev  = (8 << 8) | blk->dev_id;
    st->st_size  = blk->sector_count * 512;
    st->st_blocks = blk->sector_count;
    st->st_blksize = 512;
    return 0;
}

static int blockdev_ioctl(struct vnode *vp, uint64_t request, void *arg) {
    struct blkdev *blk = (struct blkdev *)vp->data;
    if (!blk) return -ENXIO;

    switch (request) {
    case 0x1272:
        if (arg) *(uint32_t *)arg = (uint32_t)blk->sector_count;
        return 0;
    case 0x1273:
        if (arg) *(uint64_t *)arg = blk->sector_count * 512ULL;
        return 0;
    case 0x1276:
        if (arg) *(int *)arg = 512;
        return 0;
    }
    return -ENOTTY;
}

int blockdev_register(const char *name, int dev_id, uint64_t sector_count) {
    struct vnode *dev_vn = NULL;
    int err = vfs_lookup("/dev", NULL, &dev_vn);
    if (err != 0) return err;

    struct vnode *blk_vn = NULL;
    err = dev_vn->ops->create(dev_vn, name, S_IFBLK | 0660, &blk_vn);
    vput(dev_vn);
    if (err != 0) return err;

    struct blkdev *blk = kmalloc(sizeof(struct blkdev));
    if (!blk) { vput(blk_vn); return -ENOMEM; }
    blk->dev_id = dev_id;
    blk->sector_count = sector_count;

    blk_vn->ops = &blockdev_ops;
    blk_vn->data = blk;
    
    vput(blk_vn);
    return 0;
}

int blockdev_get_devid(struct vnode *vp) {
    struct blkdev *blk = (struct blkdev *)vp->data;
    if (!blk) return -1;
    return blk->dev_id;
}

static struct vnode_ops blockdev_ops = {
    .read    = blockdev_read,
    .write   = blockdev_write,
    .getattr = blockdev_getattr,
    .ioctl   = blockdev_ioctl,
};
