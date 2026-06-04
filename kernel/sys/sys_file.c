#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/list.h>
#include <kernel/syscall.h>

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

    if (fd >= 0 && fd < MAX_FILES) {
        struct file *f = curproc->p_fd_table[fd];
        if (f && f->f_vn && strcmp(f->f_vn->v_name, "fb0") == 0) {
            size_t fb_size = g_boot_info.fb.pitch * g_boot_info.fb.height;
            if (f->f_pos >= fb_size) return 0;
            size_t actual_count = count;
            if (f->f_pos + actual_count > fb_size) {
                actual_count = fb_size - f->f_pos;
            }
            if (actual_count > 0) {
                if (copy_from_user((char *)g_boot_info.fb.fb_addr + f->f_pos, user_buf, actual_count) < 0) {
                    return -EFAULT;
                }
                f->f_pos += actual_count;
            }
            return (int64_t)actual_count;
        }
    }

    if (fd == 1 || fd == 2) {
        return tty_write(user_buf, count);
    }

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
    if (copy_from_user(kpath, user_path, 256) < 0) return -EFAULT;
    kpath[255] = '\0';

    int fd_out = -1;
    int err = vfs_open(kpath, flags, (mode_t)mode, &fd_out);
    if (err < 0) return (int64_t)err;
    return (int64_t)fd_out;
}

int64_t sys_close(int fd) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;

    struct file *f = curproc->p_fd_table[fd];
    if (!f) return -EBADF;

    curproc->p_fd_table[fd] = NULL;

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
    if (!is_user_address_range(user_buf, count)) return -EFAULT;

    struct file *f = curproc->p_fd_table[fd];
    if (f && f->f_vn && (strcmp(f->f_vn->v_name, "kbd") == 0 || strcmp(f->f_vn->v_name, "tty") == 0)) {
        return tty_read(user_buf, count);
    }

    char kbuf[4096];
    size_t total = 0;

    while (total < count) {
        size_t to_copy = count - total;
        if (to_copy > sizeof(kbuf)) to_copy = sizeof(kbuf);

        int n = vfs_read(fd, kbuf, to_copy);

        if (n < 0) return (total == 0) ? n : (int64_t)total;
        if (n == 0) break;

        if (copy_to_user((char *)user_buf + total, kbuf, n) < 0) {
            return -EFAULT;
        }
        total += n;
    }

    return (int64_t)total;
}

int64_t sys_fstat(int fd, void *user_statbuf) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!is_user_address_range(user_statbuf, sizeof(struct stat))) return -EFAULT;

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    if (fd == 0 || fd == 1 || fd == 2) {
        kst.st_mode = S_IFCHR | 0666; 
        kst.st_blksize = 1024;
        kst.st_blocks = 0;
        kst.st_size = 0;
    } else {
        struct file *f = curproc->p_fd_table[fd];
        if (!f || !f->f_vn) return -EBADF;

        kst.st_mode = S_IFREG | 0644;
        kst.st_blksize = 4096;
        kst.st_size = 0;
    }

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
    if (copy_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    if (strlen(kpath) == 0) {
        return sys_fstat(dirfd, user_statbuf);
    }

    struct vnode *vn = NULL;
    int err = vfs_lookup(kpath, curproc->p_cwd, &vn);
    if (err < 0) return err;

    struct stat kst;
    memset(&kst, 0, sizeof(struct stat));

    kst.st_mode = S_IFREG | 0644; 
    kst.st_blksize = 4096;
    kst.st_size = 0;

    vput(vn);

    if (copy_to_user(user_statbuf, &kst, sizeof(struct stat)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_ioctl(int fd, uint64_t request, void *arg) {
    if (fd >= 0 && fd < MAX_FILES) {
        struct file *f = curproc->p_fd_table[fd];
        if (f && f->f_vn) {
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
                    if (!is_user_address_range(arg, sizeof(info))) return -EFAULT;
                    if (copy_to_user(arg, &info, sizeof(info)) < 0) return -EFAULT;
                    return 0;
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
                    if (!is_user_address_range(arg, sizeof(fb_var))) return -EFAULT;
                    if (copy_to_user(arg, &fb_var, sizeof(fb_var)) < 0) return -EFAULT;
                    return 0;
                }
                else if (request == FBIOGET_FSCREENINFO) {
                    struct our_fb_fix_screeninfo fb_fix = {0};
                    strcpy(fb_fix.id, "Doppio FB");
                    fb_fix.smem_start = (unsigned long)g_boot_info.fb.fb_addr;
                    fb_fix.smem_len = g_boot_info.fb.pitch * g_boot_info.fb.height;
                    fb_fix.type = 0; // FB_TYPE_PACKED_PIXELS
                    fb_fix.visual = 2; // FB_VISUAL_TRUECOLOR
                    fb_fix.line_length = g_boot_info.fb.pitch;
                    if (!is_user_address_range(arg, sizeof(fb_fix))) return -EFAULT;
                    if (copy_to_user(arg, &fb_fix, sizeof(fb_fix)) < 0) return -EFAULT;
                    return 0;
                }
            }
            else if (strcmp(f->f_vn->v_name, "kbd") == 0 || strcmp(f->f_vn->v_name, "tty") == 0) {
                if (request == 0x4B33) { // KDGKBTYPE
                    int val = 0x02; // KB_101
                    if (!is_user_address_range(arg, sizeof(int))) return -EFAULT;
                    if (copy_to_user(arg, &val, sizeof(int)) < 0) return -EFAULT;
                    return 0;
                }
                else if (request == 0x4B44) { // KDGKBMODE
                    int val = 0x01; // K_XLATE
                    if (!is_user_address_range(arg, sizeof(int))) return -EFAULT;
                    if (copy_to_user(arg, &val, sizeof(int)) < 0) return -EFAULT;
                    return 0;
                }
                else if (request == 0x4B45) { // KDSKBMODE
                    return 0;
                }
                else if (request == 0x5401) { // TCGETS
                    struct our_termios term = {0};
                    term.c_iflag = 0x0500; // ICRNL | IXON
                    term.c_oflag = 0x0005; // OPOST | ONLCR
                    term.c_cflag = 0x0BF0; // B9600 | CS8 | CREAD | HUPCL
                    term.c_lflag = 0x8A3B; // ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN
                    if (!is_user_address_range(arg, sizeof(term))) return -EFAULT;
                    if (copy_to_user(arg, &term, sizeof(term)) < 0) return -EFAULT;
                    return 0;
                }
                else if (request == 0x5402 || request == 0x5403 || request == 0x5404) { // TCSETS / W / F
                    return 0;
                }
            }
        }
    }

    if (request == 0x5413) {
        if (!is_user_address_range(arg, 8)) {
            return -EFAULT;
        }

        unsigned short fake_winsize[4] = { 25, 80, 0, 0 };

        if (copy_to_user(arg, fake_winsize, 8) < 0) {
            return -EFAULT;
        }

        return 0;
    }

    return -ENOTTY;
}

int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;

    struct file *f = curproc->p_fd_table[fd];
    if (!f) return -EBADF;

    if (cmd == 3) { // F_GETFL
        return (int64_t)f->f_flags;
    }
    if (cmd == 4) { // F_SETFL
        f->f_flags = (f->f_flags & ~0xFFFFFFF) | (arg & 0xFFFFFFF);
        return 0;
    }
    if (cmd == 1) { // F_GETFD
        return 0;
    }
    if (cmd == 2) { // F_SETFD
        return 0;
    }

    return 0;
}

int64_t sys_mkdir(const char *user_path, int mode) {
    char kpath[256];
    if (copy_from_user(kpath, user_path, 256) < 0) return -EFAULT;
    kpath[255] = '\0';

    int err = vfs_mkdir(kpath, (mode_t)mode);
    return (int64_t)err;
}

int64_t sys_mount(const char *user_source, const char *user_target, const char *user_fstype, uint64_t flags, const void *user_data) {
    char source[256];
    char target[256];
    char fstype[64] = {0};

    if (copy_from_user(source, user_source, 256) < 0) return -EFAULT;
    source[255] = '\0';

    if (copy_from_user(target, user_target, 256) < 0) return -EFAULT;
    target[255] = '\0';

    if (user_fstype) {
        if (copy_from_user(fstype, user_fstype, 64) < 0) return -EFAULT;
        fstype[63] = '\0';
    }

    if ((flags & 0x1000) || strcmp(fstype, "bind") == 0) {
        int err = vfs_bind(source, target);
        return (int64_t)err;
    }

    return -ENOSYS;
}

int64_t sys_unlink(const char *user_path) {
    char kpath[256];
    if (copy_from_user(kpath, user_path, 256) < 0) return -EFAULT;
    kpath[255] = '\0';

    int err = vfs_unlink(kpath);
    return (int64_t)err;
}

int64_t sys_rename(const char *user_old, const char *user_new) {
    char kold[256];
    char knew[256];
    if (copy_from_user(kold, user_old, 256) < 0) return -EFAULT;
    kold[255] = '\0';

    if (copy_from_user(knew, user_new, 256) < 0) return -EFAULT;
    knew[255] = '\0';

    int err = vfs_rename(kold, knew);
    return (int64_t)err;
}
