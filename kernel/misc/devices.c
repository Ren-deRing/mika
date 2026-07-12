#include <kernel/cdev.h>
#include <kernel/fs/vnode.h>
#include <kernel/printf.h>
#include <kernel/init.h>

#include <uapi/errno.h>
#include <uapi/sys/stat.h>

extern int64_t tty_read(void *user_buf, size_t count);
extern int64_t tty_write(const void *user_buf, size_t count);

static ssize_t tty_vn_read(struct vnode *vp, void *buf, size_t n, off_t off) {
    (void)vp; (void)off;
    return tty_read(buf, n);
}

static ssize_t tty_vn_write(struct vnode *vp, const void *buf, size_t n, off_t off) {
    (void)vp; (void)off;
    return tty_write(buf, n);
}

static int tty_vn_ioctl(struct vnode *vp, uint64_t request, void *arg) {
    (void)vp;
    extern int copy_to_user(void *dst, const void *src, size_t n);

    switch (request) {
    case 0x4B33: { int v = 0x02; if (copy_to_user(arg, &v, sizeof(int)) < 0) return -EFAULT; return 0; }
    case 0x4B44: { int v = 0x01; if (copy_to_user(arg, &v, sizeof(int)) < 0) return -EFAULT; return 0; }
    case 0x4B45: return 0;
    case 0x5401: {
        struct { unsigned int c_iflag, c_oflag, c_cflag, c_lflag; } term
            = { 0x0500, 0x0005, 0x0BF0, 0x8A3B };
        if (copy_to_user(arg, &term, sizeof(term)) < 0) return -EFAULT;
        return 0;
    }
    case 0x5402: case 0x5403: case 0x5404: return 0;
    case 0x5413: { unsigned short ws[4] = { 25, 80, 0, 0 }; if (copy_to_user(arg, ws, 8) < 0) return -EFAULT; return 0; }
    case 0x5601: { char mode[6] = {0}; if (copy_to_user(arg, mode, 6) < 0) return -EFAULT; return 0; }
    case 0x5603: { struct { unsigned short a,b,c; } st = {1,0,1}; if (copy_to_user(arg, &st, sizeof(st)) < 0) return -EFAULT; return 0; }
    case 0x5602: case 0x5605: case 0x5606: case 0x5607: return 0;
    case 0x4B3A: return 0;
    case 0x4B3B: { int v = 0; if (copy_to_user(arg, &v, sizeof(int)) < 0) return -EFAULT; return 0; }
    }
    return -ENOTTY;
}

static int tty_vn_getattr(struct vnode *vp, struct stat *st) {
    st->st_mode  = S_IFCHR | 0622;
    st->st_rdev  = vp->rdev;
    st->st_size  = 0;
    st->st_blocks = 0;
    st->st_blksize = 1024;
    return 0;
}

struct vnode_ops tty_ops = {
    .read    = tty_vn_read,
    .write   = tty_vn_write,
    .ioctl   = tty_vn_ioctl,
    .getattr = tty_vn_getattr,
};

static ssize_t null_vn_read(struct vnode *vp, void *buf, size_t n, off_t off) {
    (void)vp; (void)buf; (void)off; return 0;
}

static ssize_t null_vn_write(struct vnode *vp, const void *buf, size_t n, off_t off) {
    (void)vp; (void)buf; (void)off; return n;
}

static int null_vn_getattr(struct vnode *vp, struct stat *st) {
    st->st_mode  = S_IFCHR | 0666;
    st->st_rdev  = vp->rdev;
    st->st_size  = 0;
    st->st_blocks = 0;
    st->st_blksize = 1024;
    return 0;
}

struct vnode_ops null_ops = {
    .read    = null_vn_read,
    .write   = null_vn_write,
    .getattr = null_vn_getattr,
};

static struct cdev cdev_tty  = { .major = 5,  .minor_start = 0,  .minor_count = 1,  .ops = &tty_ops };
static struct cdev cdev_kbd  = { .major = 13, .minor_start = 64, .minor_count = 1,  .ops = &tty_ops };
static struct cdev cdev_null = { .major = 1,  .minor_start = 3,  .minor_count = 1,  .ops = &null_ops };

static void char_devices_init(void) {
    cdev_register(&cdev_tty);
    cdev_register(&cdev_kbd);
    cdev_register(&cdev_null);
}

subsys_initcall(char_devices_init, PRIO_THIRD);
