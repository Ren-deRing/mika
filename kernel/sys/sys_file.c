#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <kernel/lock.h>
#include <kernel/clock.h>
#include <kernel/drm.h>


#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>

#include <uapi/sys/stat.h>
#include <uapi/fcntl.h>

#include <string.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct our_fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct our_fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct our_fb_bitfield red;
    struct our_fb_bitfield green;
    struct our_fb_bitfield blue;
    struct our_fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

struct our_fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};





struct our_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

extern int64_t tty_read(void *user_buf, size_t count);
extern int64_t tty_write(const void *user_buf, size_t count);

int64_t sys_write(int fd, const void *user_buf, size_t count) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (count == 0) return 0;
    if (!is_user_address_range(user_buf, count)) return -EFAULT;

    if (fd == 1 || fd == 2) {
        return tty_write(user_buf, count);
    }

    uint64_t pflags = spin_lock_irqsave(&curproc->p_lock);
    struct file *f = curproc->p_fd_table[fd];
    if (!f) { spin_unlock_irqrestore(&curproc->p_lock, pflags); return -EBADF; }
    const char *v_name = f->f_vn ? f->f_vn->v_name : "";
    bool is_fb0 = (strcmp(v_name, "fb0") == 0);
    bool is_tty = (strcmp(v_name, "tty") == 0);
    file_ref(f);
    spin_unlock_irqrestore(&curproc->p_lock, pflags);

    if (is_fb0) {
        size_t fb_size = g_boot_info.fb.pitch * g_boot_info.fb.height;

        file_lock(f);
        off_t pos = f->f_pos;
        if (pos >= (off_t)fb_size) {
            file_unlock(f);
            file_close(f);
            return 0;
        }
        size_t actual = count;
        if ((off_t)(pos + actual) > (off_t)fb_size) {
            actual = fb_size - pos;
        }
        file_unlock(f);

        int64_t ret = 0;
        if (actual > 0) {
            if (copy_from_user((char *)g_boot_info.fb.fb_addr + pos, user_buf, actual) < 0) {
                file_close(f);
                return -EFAULT;
            }
            file_lock(f);
            f->f_pos = pos + actual;
            file_unlock(f);
            ret = (int64_t)actual;
        }

        file_close(f);
        return ret;
    }

    if (is_tty) {
        file_close(f);
        return tty_write(user_buf, count);
    }

    file_close(f);
    return (int64_t)vfs_write(fd, user_buf, count);
}

int64_t sys_writev(int fd, const void *user_iov, int iovcnt) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -EINVAL;

    typedef struct { void *base; size_t len; } kiov_t;

    if (!is_user_address_range(user_iov, iovcnt * sizeof(kiov_t))) {
        return -EFAULT;
    }

    kiov_t *kiov = kmalloc(iovcnt * sizeof(kiov_t));
    if (!kiov) return -ENOMEM;

    if (copy_from_user(kiov, user_iov, iovcnt * sizeof(kiov_t)) < 0) {
        kfree(kiov);
        return -EFAULT;
    }

    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (kiov[i].len == 0) continue;
        int64_t r = sys_write(fd, kiov[i].base, kiov[i].len);
        if (r < 0) {
            kfree(kiov);
            return total > 0 ? total : r;
        }
        total += r;
    }

    kfree(kiov);
    return total;
}

int64_t sys_readv(int fd, const void *user_iov, int iovcnt) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -EINVAL;

    typedef struct { void *base; size_t len; } kiov_t;

    if (!is_user_address_range(user_iov, iovcnt * sizeof(kiov_t))) {
        return -EFAULT;
    }

    kiov_t *kiov = kmalloc(iovcnt * sizeof(kiov_t));
    if (!kiov) return -ENOMEM;

    if (copy_from_user(kiov, user_iov, iovcnt * sizeof(kiov_t)) < 0) {
        kfree(kiov);
        return -EFAULT;
    }

    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (kiov[i].len == 0) continue;
        int64_t r = sys_read(fd, kiov[i].base, kiov[i].len);
        if (r < 0) {
            kfree(kiov);
            return total > 0 ? total : r;
        }
        total += r;
        if (r < (int64_t)kiov[i].len) {
            break;
        }
    }

    kfree(kiov);
    return total;
}

int64_t sys_open(const char *user_path, int flags, int mode) {
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    dprintf("[sys_open] path: %s, flags: 0x%x, mode: 0x%x\n", kpath, flags, mode);

    int fd_out = -1;
    int err = vfs_open(kpath, flags, (mode_t)mode, &fd_out);
    if (err < 0) {
        dprintf("[sys_open] Failed to open %s, error: %d\n", kpath, err);
        return (int64_t)err;
    }
    dprintf("[sys_open] Succeeded path: %s, fd: %d\n", kpath, fd_out);
    return (int64_t)fd_out;
}

int64_t sys_close(int fd) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;

    uint64_t flags = spin_lock_irqsave(&curproc->p_lock);
    struct file *f = curproc->p_fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&curproc->p_lock, flags);
        return -EBADF;
    }
    curproc->p_fd_table[fd] = NULL;
    spin_unlock_irqrestore(&curproc->p_lock, flags);

    file_close(f);
    return 0;
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    int64_t res = (int64_t)vfs_lseek(fd, (off_t)offset, whence);
    return res;
}

int64_t sys_read(int fd, void *user_buf, size_t count) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (count == 0) return 0;
    if (!is_user_address_range(user_buf, count)) {
        dprintf("[sys_read] Invalid user address range: fd=%d, buf=%p, count=%lu\n", fd, user_buf, count);
        return -EFAULT;
    }

    uint64_t pflags = spin_lock_irqsave(&curproc->p_lock);
    struct file *f = curproc->p_fd_table[fd];
    if (!f) { spin_unlock_irqrestore(&curproc->p_lock, pflags); return -EBADF; }
    const char *v_name = f->f_vn ? f->f_vn->v_name : "";
    bool is_special = (strcmp(v_name, "kbd") == 0 || strcmp(v_name, "tty") == 0);
    bool is_card0 = (strcmp(v_name, "card0") == 0);
    spin_unlock_irqrestore(&curproc->p_lock, pflags);

    if (is_special) {
        return tty_read(user_buf, count);
    }
    if (is_card0) {
        return drm_read(f, user_buf, count);
    }

    char kbuf[4096];
    size_t total = 0;

    while (total < count) {
        size_t to_copy = count - total;
        if (to_copy > sizeof(kbuf)) to_copy = sizeof(kbuf);

        int n = vfs_read(fd, kbuf, to_copy);

        if (n < 0) {
            if (is_gbm) dprintf("[sys_read] libgbm.so read error: %d\n", n);
            return (total == 0) ? n : (int64_t)total;
        }
        if (n == 0) break;

        if (is_gbm) {
            dprintf("[sys_read] libgbm.so read %d bytes (requested %ld), first 4 bytes: %02x %02x %02x %02x\n",
                    n, to_copy, (unsigned char)kbuf[0], (unsigned char)kbuf[1], (unsigned char)kbuf[2], (unsigned char)kbuf[3]);
        }

        if (copy_to_user((char *)user_buf + total, kbuf, n) < 0) {
            return -EFAULT;
        }
        total += n;
    }

    return (int64_t)total;
}

static void fill_rdev(struct vnode *vn, struct stat *st) {
    if (vn && S_ISCHR(st->st_mode)) {
        if (strcmp(vn->v_name, "tty") == 0) {
            st->st_rdev = (5 << 8) | 0;
        } else if (strcmp(vn->v_name, "card0") == 0) {
            st->st_rdev = (226 << 8) | 0;
        } else if (strcmp(vn->v_name, "null") == 0) {
            st->st_rdev = (1 << 8) | 3;
        } else if (strcmp(vn->v_name, "fb0") == 0) {
            st->st_rdev = (29 << 8) | 0;
        } else if (strcmp(vn->v_name, "kbd") == 0) {
            st->st_rdev = (13 << 8) | 64;
        }
    }
}

int64_t sys_fstat(int fd, void *user_statbuf) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!is_user_address_range(user_statbuf, sizeof(struct stat))) return -EFAULT;

    struct file *f = fdget(fd);
    if (!f) {
        if (fd == 0 || fd == 1 || fd == 2) {
            struct stat kst;
            memset(&kst, 0, sizeof(struct stat));
            kst.st_mode = S_IFCHR | 0666;
            kst.st_blksize = 1024;
            kst.st_blocks = 0;
            kst.st_size = 0;
            kst.st_dev = 1;
            kst.st_ino = fd + 1;
            kst.st_rdev = (5 << 8) | 0;
            if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) return -EFAULT;
            return 0;
        }
        return -EBADF;
    }

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    if (f->f_vn) {
        struct vnode *vp = f->f_vn;
        if (vp->ops && vp->ops->getattr) {
            int r = vp->ops->getattr(vp, &kst);
            if (r < 0) {
                dprintf("[sys_fstat] getattr failed for fd %d: %d\n", fd, r);
                fdput(f);
                return r;
            }
        } else {
            kst.st_mode = vp->type;
            kst.st_size = 0;
        }
        if ((kst.st_mode & 0777) == 0) {
            if (S_ISDIR(kst.st_mode)) {
                kst.st_mode |= 0755;
            } else {
                kst.st_mode |= 0644;
            }
        }
        kst.st_blksize = 4096;
        kst.st_dev = 1;
        kst.st_ino = (ino_t)vp;
        kst.st_blocks = (kst.st_size + 511) / 512;
        fill_rdev(vp, &kst);
        dprintf("[sys_fstat] fd: %d (%s), mode: 0x%x, size: %ld, blksize: %ld, dev: %ld, ino: %ld, rdev: %ld\n", fd, vp->v_name, kst.st_mode, kst.st_size, kst.st_blksize, kst.st_dev, kst.st_ino, kst.st_rdev);
    }

    fdput(f);

    if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_newfstatat(int dirfd, const char *user_path, void *user_statbuf, int flags) {
    if (!is_user_address_range(user_statbuf, sizeof(struct stat))) return -EFAULT;

    if (flags & AT_EMPTY_PATH) {
        return sys_fstat(dirfd, user_statbuf);
    }

    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    if (strlen(kpath) == 0) {
        return sys_fstat(dirfd, user_statbuf);
    }

    char clean_path[256];
    sanitize_path(kpath, clean_path, sizeof(clean_path));

    struct vnode *vn = NULL;
    int err = vfs_lookup(clean_path, curproc->p_cwd, &vn);
    if (err < 0) return err;

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    if (vn->ops && vn->ops->getattr) {
        int r = vn->ops->getattr(vn, &kst);
        if (r < 0) {
            vput(vn);
            return r;
        }
    } else {
        kst.st_mode = vn->type;
        kst.st_size = 0;
    }
    if ((kst.st_mode & 0777) == 0) {
        if (S_ISDIR(kst.st_mode)) {
            kst.st_mode |= 0755;
        } else {
            kst.st_mode |= 0644;
        }
    }
    kst.st_blksize = 4096;
    kst.st_dev = 1;
    kst.st_ino = (ino_t)vn;
    kst.st_blocks = (kst.st_size + 511) / 512;
    fill_rdev(vn, &kst);

    vput(vn);

    if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_stat(const char *user_path, void *user_statbuf) {
    if (!is_user_address_range(user_statbuf, sizeof(struct stat))) return -EFAULT;

    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    char clean_path[256];
    sanitize_path(kpath, clean_path, sizeof(clean_path));

    struct vnode *vn = NULL;
    int err = vfs_lookup(clean_path, curproc->p_cwd, &vn);
    if (err < 0) return err;

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    if (vn->ops && vn->ops->getattr) {
        int r = vn->ops->getattr(vn, &kst);
        if (r < 0) {
            vput(vn);
            return r;
        }
    } else {
        kst.st_mode = vn->type;
        kst.st_size = 0;
    }
    if ((kst.st_mode & 0777) == 0) {
        if (S_ISDIR(kst.st_mode)) {
            kst.st_mode |= 0755;
        } else {
            kst.st_mode |= 0644;
        }
    }
    kst.st_blksize = 4096;
    kst.st_dev = 1;
    kst.st_ino = (ino_t)vn;
    kst.st_blocks = (kst.st_size + 511) / 512;
    fill_rdev(vn, &kst);

    vput(vn);

    if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_lstat(const char *user_path, void *user_statbuf) {
    return sys_stat(user_path, user_statbuf);
}

int64_t sys_readlink(const char *user_path, char *user_buf, size_t user_bufsiz) {
    if (user_bufsiz == 0) return 0;
    if (!is_user_address_range(user_buf, user_bufsiz)) {
        return -EFAULT;
    }

    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    char clean_path[256];
    sanitize_path(kpath, clean_path, sizeof(clean_path));

    struct vnode *vn = NULL;
    int err = vfs_lookup_impl(clean_path, curproc->p_cwd, 0, 0, &vn);
    if (err < 0) {
        return (int64_t)err;
    }

    if (vn->type != S_IFLNK) {
        vput(vn);
        return -EINVAL;
    }

    char *target_buf = kmalloc(512);
    if (!target_buf) {
        vput(vn);
        return -ENOMEM;
    }

    ssize_t n = vn->ops->read(vn, target_buf, 511, 0);
    vput(vn);

    if (n < 0) {
        kfree(target_buf);
        return n;
    }
    target_buf[n] = '\0';

    size_t to_copy = (n < (ssize_t)user_bufsiz) ? (size_t)n : user_bufsiz;
    if (copy_to_user(user_buf, target_buf, to_copy) < 0) {
        kfree(target_buf);
        return -EFAULT;
    }

    kfree(target_buf);
    return (int64_t)to_copy;
}

int64_t sys_symlink(const char *user_target, const char *user_linkpath) {
    char *ktarget = kmalloc(512);
    char *klinkpath = kmalloc(256);
    if (!ktarget || !klinkpath) {
        if (ktarget) kfree(ktarget);
        if (klinkpath) kfree(klinkpath);
        return -ENOMEM;
    }

    if (copy_str_from_user(ktarget, user_target, 512) < 0 ||
        copy_str_from_user(klinkpath, user_linkpath, 256) < 0) {
        kfree(ktarget);
        kfree(klinkpath);
        return -EFAULT;
    }

    int err = vfs_symlink(ktarget, klinkpath);

    kfree(ktarget);
    kfree(klinkpath);
    return err;
}

int64_t sys_flock(int fd, int operation) {
    (void)operation;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    fdput(f);
    return 0;
}

int64_t sys_ioctl_impl(int fd, uint64_t request_raw, void *arg);

int64_t sys_ioctl(int fd, uint64_t request_raw, void *arg) {
    int64_t ret = sys_ioctl_impl(fd, request_raw, arg);
    const char *name = "unknown";
    if (fd >= 0 && fd < MAX_FILES) {
        struct file *f = curproc->p_fd_table[fd];
        if (f && f->f_vn) {
            name = f->f_vn->v_name;
        } else if (fd == 0 || fd == 1 || fd == 2) {
            name = "fallback-tty";
        }
    }
    dprintf("[KERNEL sys_ioctl] fd=%d (%s), request=0x%lx, arg=%p -> ret=%ld\n", fd, name, request_raw, arg, ret);
    return ret;
}

int64_t sys_ioctl_impl(int fd, uint64_t request_raw, void *arg) {
    uint32_t request = (uint32_t)request_raw;
    int64_t ret = -ENOTTY;

    struct file *f = fdget(fd);
    if (!f) {
        if (fd == 0 || fd == 1 || fd == 2) {
            if (request == 0x5401) {
                struct our_termios term = {0};
                term.c_iflag = 0x0500;
                term.c_oflag = 0x0005;
                term.c_cflag = 0x0BF0;
                term.c_lflag = 0x8A3B;
                if (copy_to_user(arg, &term, sizeof(term)) < 0) return -EFAULT;
                return 0;
            }
            if (request == 0x5402 || request == 0x5403 || request == 0x5404) return 0;
            if (request == 0x5413) {
                unsigned short fake_winsize[4] = { 25, 80, 0, 0 };
                if (copy_to_user(arg, fake_winsize, 8) < 0) return -EFAULT;
                return 0;
            }
        }
        return -EBADF;
    }

    if (f->f_vn) {
        dprintf("[KERNEL sys_ioctl] fd=%d, name='%s', request=0x%x (raw=0x%lx), arg=%p\n", fd, f->f_vn->v_name, request, request_raw, arg);
        if (f->f_vn->ops->ioctl) {
            ret = f->f_vn->ops->ioctl(f->f_vn, (uint64_t)request, arg);
            goto out;
        }

        if (strcmp(f->f_vn->v_name, "fb0") == 0) {
            if (request == 0x4601) {
                struct {
                    uint32_t width, height, pitch, bpp;
                } info = {
                    g_boot_info.fb.width, g_boot_info.fb.height,
                    g_boot_info.fb.pitch, g_boot_info.fb.bpp
                };
                if (!is_user_address_range(arg, sizeof(info))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &info, sizeof(info)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
            else if (request == FBIOGET_VSCREENINFO) {
                struct our_fb_var_screeninfo fb_var = {0};
                fb_var.xres = g_boot_info.fb.width;
                fb_var.yres = g_boot_info.fb.height;
                fb_var.xres_virtual = g_boot_info.fb.width;
                fb_var.yres_virtual = g_boot_info.fb.height;
                fb_var.bits_per_pixel = g_boot_info.fb.bpp;
                if (g_boot_info.fb.bpp == 32) {
                    fb_var.red.offset = 16;    fb_var.red.length = 8;
                    fb_var.green.offset = 8;   fb_var.green.length = 8;
                    fb_var.blue.offset = 0;    fb_var.blue.length = 8;
                    fb_var.transp.offset = 24; fb_var.transp.length = 8;
                } else if (g_boot_info.fb.bpp == 16) {
                    fb_var.red.offset = 11;    fb_var.red.length = 5;
                    fb_var.green.offset = 5;   fb_var.green.length = 6;
                    fb_var.blue.offset = 0;    fb_var.blue.length = 5;
                }
                if (!is_user_address_range(arg, sizeof(fb_var))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &fb_var, sizeof(fb_var)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
            else if (request == FBIOGET_FSCREENINFO) {
                struct our_fb_fix_screeninfo fb_fix = {0};
                strcpy(fb_fix.id, "Doppio FB");
                fb_fix.smem_start = (unsigned long)g_boot_info.fb.fb_addr;
                fb_fix.smem_len = g_boot_info.fb.pitch * g_boot_info.fb.height;
                fb_fix.type = 0;
                fb_fix.visual = 2;
                fb_fix.line_length = g_boot_info.fb.pitch;
                if (!is_user_address_range(arg, sizeof(fb_fix))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &fb_fix, sizeof(fb_fix)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
        }
        else if (strcmp(f->f_vn->v_name, "card0") == 0) {
            ret = drm_ioctl(f, request, arg);
            goto out;
        }
        else if (strcmp(f->f_vn->v_name, "kbd") == 0 || strcmp(f->f_vn->v_name, "tty") == 0) {
            if (request == 0x4B33) {
                int val = 0x02;
                if (!is_user_address_range(arg, sizeof(int))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &val, sizeof(int)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
            else if (request == 0x4B44) {
                int val = 0x01;
                if (!is_user_address_range(arg, sizeof(int))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &val, sizeof(int)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
            else if (request == 0x4B45) { ret = 0; goto out; }
            else if (request == 0x5401) {
                struct our_termios term = {0};
                term.c_iflag = 0x0500; term.c_oflag = 0x0005;
                term.c_cflag = 0x0BF0; term.c_lflag = 0x8A3B;
                if (!is_user_address_range(arg, sizeof(term))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &term, sizeof(term)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
            else if (request == 0x5402 || request == 0x5403 || request == 0x5404) { ret = 0; goto out; }
            else if (request == 0x5603) {
                struct { unsigned short v_active, v_signal, v_state; } state = {0};
                state.v_active = 1; state.v_state = 1;
                if (!is_user_address_range(arg, sizeof(state))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &state, sizeof(state)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
            else if (request == 0x5601) {
                struct { char mode, waitv; short relsig; char acqsig, frsig; } mode = {0};
                if (!is_user_address_range(arg, sizeof(mode))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &mode, sizeof(mode)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
            else if (request == 0x5602 || request == 0x5605 || request == 0x5606 || request == 0x5607) { ret = 0; goto out; }
            else if (request == 0x4B3A) { ret = 0; goto out; }
            else if (request == 0x4B3B) {
                int val = 0;
                if (!is_user_address_range(arg, sizeof(int))) { ret = -EFAULT; goto out; }
                if (copy_to_user(arg, &val, sizeof(int)) < 0) { ret = -EFAULT; goto out; }
                ret = 0; goto out;
            }
        }
    }

    if (request == 0x5413) {
        if (!is_user_address_range(arg, 8)) {
            ret = -EFAULT;
            goto out;
        }
        unsigned short fake_winsize[4] = { 25, 80, 0, 0 };
        if (copy_to_user(arg, fake_winsize, 8) < 0) {
            ret = -EFAULT;
            goto out;
        }
        ret = 0;
        goto out;
    }

out:
    fdput(f);
    return ret;
}

int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;

    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    if (cmd == 0 || cmd == 1030) {
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= MAX_FILES) { fdput(f); return -EINVAL; }

        uint64_t pflags = spin_lock_irqsave(&curproc->p_lock);
        int newfd = -1;
        for (int i = minfd; i < MAX_FILES; i++) {
            if (curproc->p_fd_table[i] == NULL) {
                curproc->p_fd_table[i] = f;
                __atomic_fetch_add(&f->f_refcnt, 1, __ATOMIC_SEQ_CST);
                newfd = i;
                break;
            }
        }
        spin_unlock_irqrestore(&curproc->p_lock, pflags);

        fdput(f);
        if (newfd == -1) return -EMFILE;
        return (int64_t)newfd;
    }

    int64_t ret = 0;

    if (cmd == 3) {
        file_lock(f);
        ret = (int64_t)f->f_flags;
        file_unlock(f);
    } else if (cmd == 4) {
        file_lock(f);
        f->f_flags = (f->f_flags & ~0xFFFFFFF) | (arg & 0xFFFFFFF);
        file_unlock(f);
    }

    fdput(f);
    return ret;
}

int64_t sys_mkdir(const char *user_path, int mode) {
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    int err = vfs_mkdir(kpath, (mode_t)mode);
    return (int64_t)err;
}

int64_t sys_mount(const char *user_source, const char *user_target, const char *user_fstype, uint64_t flags, const void *user_data) {
    char source[256];
    char target[256];
    char fstype[64] = {0};

    if (copy_str_from_user(source, user_source, 256) < 0) return -EFAULT;

    if (copy_str_from_user(target, user_target, 256) < 0) return -EFAULT;

    if (user_fstype) {
        if (copy_str_from_user(fstype, user_fstype, 64) < 0) return -EFAULT;
    }

    if ((flags & 0x1000) || strcmp(fstype, "bind") == 0) {
        int err = vfs_bind(source, target);
        return (int64_t)err;
    }

    return -ENOSYS;
}

int64_t sys_unlink(const char *user_path) {
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    int err = vfs_unlink(kpath);
    return (int64_t)err;
}

int64_t sys_rename(const char *user_old, const char *user_new) {
    char kold[256];
    char knew[256];
    if (copy_str_from_user(kold, user_old, 256) < 0) return -EFAULT;

    if (copy_str_from_user(knew, user_new, 256) < 0) return -EFAULT;

    int err = vfs_rename(kold, knew);
    return (int64_t)err;
}

int64_t sys_access(const char *user_path, int mode) {
    (void)mode;
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    char clean_path[256];
    sanitize_path(kpath, clean_path, sizeof(clean_path));

    struct vnode *vn = NULL;
    int err = vfs_lookup(clean_path, curproc->p_cwd, &vn);
    if (err < 0) {
        return (int64_t)err;
    }

    vput(vn);
    return 0;
}

int64_t sys_faccessat(int dirfd, const char *user_path, int mode, int flags) {
    (void)dirfd;
    (void)flags;
    return sys_access(user_path, mode);
}

int64_t sys_faccessat2(int dirfd, const char *user_path, int mode, int flags) {
    (void)dirfd;
    (void)flags;
    return sys_access(user_path, mode);
}

int64_t sys_getdents64(int fd, void *user_buf, size_t count) {
    if (!is_user_address_range(user_buf, count)) return -EFAULT;

    void *kbuf = kmalloc(count);
    if (!kbuf) return -ENOMEM;
    memset(kbuf, 0, count);

    extern int vfs_readdir(int fd, void *buf, size_t count);
    int ret = vfs_readdir(fd, kbuf, count);
    if (ret < 0) {
        kfree(kbuf);
        return ret;
    }

    if (copy_to_user(user_buf, kbuf, ret) < 0) {
        kfree(kbuf);
        return -EFAULT;
    }

    kfree(kbuf);
    return ret;
}

int64_t sys_memfd_create(const char *user_name, unsigned int flags) {
    char kname[256];
    if (user_name) {
        if (copy_str_from_user(kname, user_name, sizeof(kname)) < 0) return -EFAULT;
    } else {
        strcpy(kname, "memfd");
    }

    static int memfd_id = 0;
    char path[512];
    snprintf(path, sizeof(path), "/tmp/memfd_%d_%s", __sync_fetch_and_add(&memfd_id, 1), kname);

    dprintf("[sys_memfd_create] Creating memfd: %s, flags: 0x%x\n", path, flags);

    int fd_out = -1;
    int open_flags = O_RDWR | O_CREAT;
    int err = vfs_open(path, open_flags, 0600, &fd_out);
    if (err < 0) {
        dprintf("[sys_memfd_create] Failed to create memfd: %s, error: %d\n", path, err);
        return (int64_t)err;
    }

    dprintf("[sys_memfd_create] Succeeded: fd=%d\n", fd_out);
    return (int64_t)fd_out;
}
