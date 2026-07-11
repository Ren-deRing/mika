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

extern void fill_rdev(struct vnode *vn, struct stat *st);

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
    if (count == 0) return 0;
    if (!is_user_address_range(user_buf, count)) return -EFAULT;

    struct file *f = fdget(fd);
    if (!f) {
        if (fd == 1 || fd == 2) {
            return tty_write(user_buf, count);
        }
        return -EBADF;
    }

    if (f->f_vn && strcmp(f->f_vn->v_name, "fb0") == 0) {
        size_t fb_size = g_boot_info.fb.pitch * g_boot_info.fb.height;
        if (f->f_pos >= fb_size) { fdput(f); return 0; }
        size_t actual_count = count;
        if (f->f_pos + actual_count > fb_size) {
            actual_count = fb_size - f->f_pos;
        }
        if (actual_count > 0) {
            if (copy_from_user((char *)g_boot_info.fb.fb_addr + f->f_pos, user_buf, actual_count) < 0) {
                fdput(f); return -EFAULT;
            }
            f->f_pos += actual_count;
        }
        fdput(f);
        return (int64_t)actual_count;
    }

    if (f->f_vn) {
        if (strcmp(f->f_vn->v_name, "tty") == 0) {
            fdput(f);
            return tty_write(user_buf, count);
        }
        int64_t ret = vfs_write(fd, user_buf, count);
        fdput(f);
        return ret;
    }

    fdput(f);
    if (fd == 1 || fd == 2) {
        return tty_write(user_buf, count);
    }
    return -EBADF;
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
    if (!f) { spin_unlock_irqrestore(&curproc->p_lock, flags); return -EBADF; }
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
    if (count == 0) return 0;
    if (!is_user_address_range(user_buf, count)) {
        return -EFAULT;
    }

    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    if (f->f_vn) {
        if (strcmp(f->f_vn->v_name, "kbd") == 0 || strcmp(f->f_vn->v_name, "tty") == 0) {
            fdput(f);
            return tty_read(user_buf, count);
        }
        if (strcmp(f->f_vn->v_name, "card0") == 0) {
            int64_t ret = drm_read(f, user_buf, count);
            fdput(f);
            return ret;
        }
    }

    char kbuf[4096];
    size_t total = 0;

    while (total < count) {
        size_t to_copy = count - total;
        if (to_copy > sizeof(kbuf)) to_copy = sizeof(kbuf);

        int n = vfs_read(fd, kbuf, to_copy);

        if (n < 0) {
            fdput(f);
            return (total == 0) ? n : (int64_t)total;
        }
        if (n == 0) break;

        if (copy_to_user((char *)user_buf + total, kbuf, n) < 0) {
            fdput(f);
            return -EFAULT;
        }
        total += n;
    }

    fdput(f);
    return (int64_t)total;
}

int64_t sys_fstat(int fd, void *user_statbuf) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    if (!is_user_address_range(user_statbuf, sizeof(struct stat))) { fdput(f); return -EFAULT; }

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    if (f->f_vn) {
        struct vnode *vp = f->f_vn;
        if (vp->ops && vp->ops->getattr) {
            int r = vp->ops->getattr(vp, &kst);
            if (r < 0) {
                dprintf("[sys_fstat] getattr failed for fd %d: %d\n", fd, r);
                fdput(f); return r;
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
    } else if (fd == 0 || fd == 1 || fd == 2) {
        kst.st_mode = S_IFCHR | 0666; 
        kst.st_blksize = 1024;
        kst.st_blocks = 0;
        kst.st_size = 0;
        kst.st_dev = 1;
        kst.st_ino = fd + 1;
        kst.st_rdev = (5 << 8) | 0;
        dprintf("[sys_fstat] fd: %d (fallback), mode: 0x%x, size: %ld, blksize: %ld, dev: %ld, ino: %ld, rdev: %ld\n", fd, kst.st_mode, kst.st_size, kst.st_blksize, kst.st_dev, kst.st_ino, kst.st_rdev);
    } else {
        dprintf("[sys_fstat] fd: %d (invalid/closed)\n", fd);
        fdput(f); return -EBADF;
    }

    if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) {
        fdput(f); return -EFAULT;
    }

    fdput(f);
    return 0;
}

int64_t sys_flock(int fd, int operation) {
    (void)operation;
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    fdput(f);
    return 0;
}

int64_t sys_ioctl_impl(int fd, uint64_t request_raw, void *arg);

int64_t sys_ioctl(int fd, uint64_t request_raw, void *arg) {
    int64_t ret = sys_ioctl_impl(fd, request_raw, arg);
    const char *name = "unknown";
    struct file *f = fdget(fd);
    if (f) {
        if (f->f_vn) {
            name = f->f_vn->v_name;
        }
        fdput(f);
    } else if (fd == 0 || fd == 1 || fd == 2) {
        name = "fallback-tty";
    }
    dprintf("[KERNEL sys_ioctl] fd=%d (%s), request=0x%lx, arg=%p -> ret=%ld\n", fd, name, request_raw, arg, ret);
    return ret;
}

int64_t sys_ioctl_impl(int fd, uint64_t request_raw, void *arg) {
    uint32_t request = (uint32_t)request_raw;
    struct file *f = fdget(fd);
    int64_t ret = -ENOTTY;

    if (f && f->f_vn) {
        dprintf("[KERNEL sys_ioctl] fd=%d, name='%s', request=0x%x (raw=0x%lx), arg=%p\n", fd, f->f_vn->v_name, request, request_raw, arg);
        if (f->f_vn->ops->ioctl) {
            ret = f->f_vn->ops->ioctl(f->f_vn, (uint64_t)request, arg);
            goto done;
        }
        if (strcmp(f->f_vn->v_name, "fb0") == 0) {
            if (request == 0x4601) { // FBIOGET_INFO
                struct {
                    uint32_t width;
                    uint32_t height;
                    uint32_t pitch;
                    uint32_t bpp;
                } info = {
                    g_boot_info.fb.width,
                    g_boot_info.fb.height,
                    g_boot_info.fb.pitch,
                    g_boot_info.fb.bpp
                };
                if (!is_user_address_range(arg, sizeof(info))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &info, sizeof(info)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
            else if (request == FBIOGET_VSCREENINFO) {
                struct our_fb_var_screeninfo fb_var = {0};
                fb_var.xres = g_boot_info.fb.width;
                fb_var.yres = g_boot_info.fb.height;
                fb_var.xres_virtual = g_boot_info.fb.width;
                fb_var.yres_virtual = g_boot_info.fb.height;
                fb_var.bits_per_pixel = g_boot_info.fb.bpp;
                if (g_boot_info.fb.bpp == 32) {
                    fb_var.red.offset = 16;
                    fb_var.red.length = 8;
                    fb_var.green.offset = 8;
                    fb_var.green.length = 8;
                    fb_var.blue.offset = 0;
                    fb_var.blue.length = 8;
                    fb_var.transp.offset = 24;
                    fb_var.transp.length = 8;
                } else if (g_boot_info.fb.bpp == 16) {
                    fb_var.red.offset = 11;
                    fb_var.red.length = 5;
                    fb_var.green.offset = 5;
                    fb_var.green.length = 6;
                    fb_var.blue.offset = 0;
                    fb_var.blue.length = 5;
                }
                if (!is_user_address_range(arg, sizeof(fb_var))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &fb_var, sizeof(fb_var)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
            else if (request == FBIOGET_FSCREENINFO) {
                struct our_fb_fix_screeninfo fb_fix = {0};
                strcpy(fb_fix.id, "Doppio FB");
                fb_fix.smem_start = (unsigned long)g_boot_info.fb.fb_addr;
                fb_fix.smem_len = g_boot_info.fb.pitch * g_boot_info.fb.height;
                fb_fix.type = 0; // FB_TYPE_PACKED_PIXELS
                fb_fix.visual = 2; // FB_VISUAL_TRUECOLOR
                fb_fix.line_length = g_boot_info.fb.pitch;
                if (!is_user_address_range(arg, sizeof(fb_fix))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &fb_fix, sizeof(fb_fix)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
        }
        else if (strcmp(f->f_vn->v_name, "card0") == 0) {
            ret = drm_ioctl(f, request, arg);
            goto done;
        }
        else if (strcmp(f->f_vn->v_name, "kbd") == 0 || strcmp(f->f_vn->v_name, "tty") == 0) {
            if (request == 0x4B33) { // KDGKBTYPE
                int val = 0x02; // KB_101
                if (!is_user_address_range(arg, sizeof(int))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &val, sizeof(int)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
            else if (request == 0x4B44) { // KDGKBMODE
                int val = 0x01; // K_XLATE
                if (!is_user_address_range(arg, sizeof(int))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &val, sizeof(int)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
            else if (request == 0x4B45) { // KDSKBMODE
                ret = 0; goto done;
            }
            else if (request == 0x5401) { // TCGETS
                struct our_termios term = {0};
                term.c_iflag = 0x0500; // ICRNL | IXON
                term.c_oflag = 0x0005; // OPOST | ONLCR
                term.c_cflag = 0x0BF0; // B9600 | CS8 | CREAD | HUPCL
                term.c_lflag = 0x8A3B; // ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN
                if (!is_user_address_range(arg, sizeof(term))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &term, sizeof(term)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
            else if (request == 0x5402 || request == 0x5403 || request == 0x5404) { // TCSETS / W / F
                ret = 0; goto done;
            }
            else if (request == 0x5603) { // VT_GETSTATE
                struct {
                    unsigned short v_active;
                    unsigned short v_signal;
                    unsigned short v_state;
                } state = {0};
                state.v_active = 1;
                state.v_state = 1;
                if (!is_user_address_range(arg, sizeof(state))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &state, sizeof(state)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
            else if (request == 0x5601) { // VT_GETMODE
                struct {
                    char mode;
                    char waitv;
                    short relsig;
                    char acqsig;
                    char frsig;
                } mode = {0};
                mode.mode = 0;
                if (!is_user_address_range(arg, sizeof(mode))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &mode, sizeof(mode)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
            else if (request == 0x5602 || request == 0x5605 || request == 0x5606 || request == 0x5607) { // VT_SETMODE / VT_RELDISP / VT_ACTIVATE / VT_WAITACTIVE
                ret = 0; goto done;
            }
            else if (request == 0x4B3A) { // KDSETMODE
                ret = 0; goto done;
            }
            else if (request == 0x4B3B) { // KDGETMODE
                int val = 0;
                if (!is_user_address_range(arg, sizeof(int))) { ret = -EFAULT; goto done; }
                if (copy_to_user(arg, &val, sizeof(int)) < 0) { ret = -EFAULT; goto done; }
                ret = 0; goto done;
            }
        }
    }

    if (request == 0x5413) {
        if (!is_user_address_range(arg, 8)) {
            ret = -EFAULT;
            goto done;
        }

        unsigned short fake_winsize[4] = { 25, 80, 0, 0 };

        if (copy_to_user(arg, fake_winsize, 8) < 0) {
            ret = -EFAULT;
            goto done;
        }

        ret = 0;
        goto done;
    }

done:
    fdput(f);
    return ret;
}

int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    if (cmd == 0 || cmd == 1030) { // F_DUPFD or F_DUPFD_CLOEXEC
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= MAX_FILES) { fdput(f); return -EINVAL; }

        spin_lock(&curproc->p_lock);
        int newfd = -1;
        for (int i = minfd; i < MAX_FILES; i++) {
            if (curproc->p_fd_table[i] == NULL) {
                curproc->p_fd_table[i] = f;
                __atomic_fetch_add(&f->f_refcnt, 1, __ATOMIC_SEQ_CST);
                newfd = i;
                break;
            }
        }
        spin_unlock(&curproc->p_lock);

        fdput(f);
        if (newfd == -1) return -EMFILE;
        return (int64_t)newfd;
    }

    if (cmd == 3) { // F_GETFL
        int64_t ret = (int64_t)f->f_flags;
        fdput(f);
        return ret;
    }
    if (cmd == 4) { // F_SETFL
        f->f_flags = (f->f_flags & ~0xFFFFFFF) | (arg & 0xFFFFFFF);
        fdput(f);
        return 0;
    }
    if (cmd == 1) { // F_GETFD
        fdput(f);
        return 0;
    }
    if (cmd == 2) { // F_SETFD
        fdput(f);
        return 0;
    }

    fdput(f);
    return 0;
}

int64_t sys_ftruncate(int fd, int64_t length) {
    if (length < 0) return -EINVAL;

    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    struct vnode *mapped_vn = f->f_vn;
    if (!mapped_vn) { fdput(f); return -EBADF; }
    vref(mapped_vn);
    fdput(f);

    int64_t ret = 0;

    if (mapped_vn->ops && mapped_vn->ops->setattr) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_size = length;
        ret = mapped_vn->ops->setattr(mapped_vn, &st);
    } else {
        ret = -EINVAL;
    }

    vput(mapped_vn);
    
    return ret;
}

int64_t sys_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    (void)mode;
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    if (!f->f_vn) { fdput(f); return -EBADF; }

    if (offset < 0 || len < 0) { fdput(f); return -EINVAL; }

    int64_t required_size = offset + len;
    struct stat st;
    if (f->f_vn->ops->getattr) {
        int err = f->f_vn->ops->getattr(f->f_vn, &st);
        if (err < 0) { fdput(f); return err; }
        if (st.st_size < required_size) {
            if (f->f_vn->ops->setattr) {
                memset(&st, 0, sizeof(st));
                st.st_size = required_size;
                int64_t ret = f->f_vn->ops->setattr(f->f_vn, &st);
                fdput(f);
                return ret;
            }
        }
    }
    fdput(f);
    return 0;
}

