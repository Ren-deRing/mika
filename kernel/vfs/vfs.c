/*
 * Mika Kernel Virtual File System
 *
 * Copyright (c) 2026 Ren-deRing (JONGHYUN WON)
 * 
 * SPDX-License-Identifier: 0BSD
 */

#include <boot/bootinfo.h>

#include <kernel/fs/vfs.h>
#include <kernel/fs/ramfs.h>
#include <kernel/fs/vnode.h>
#include <kernel/kmem.h>
#include <kernel/init.h>
#include <kernel/printf.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>

#include <uapi/fcntl.h>
#include <uapi/errno.h>
#include <uapi/sys/stat.h>
#include <uapi/sys/dirent.h>

#include <string.h>

struct vnode *g_root_vnode = NULL;

void vfs_init(void) {
    dprintf("VFS: Initializing...\n");
    vnode_init();

    g_root_vnode = ramfs_create_vnode(S_IFDIR);
    if (!g_root_vnode) return;

    g_root_vnode->ops->mkdir(g_root_vnode, "bin", 0755);
    g_root_vnode->ops->mkdir(g_root_vnode, "etc", 0755);
    g_root_vnode->ops->mkdir(g_root_vnode, "dev", 0755);
    g_root_vnode->ops->mkdir(g_root_vnode, "tmp", 1777);
    g_root_vnode->ops->mkdir(g_root_vnode, "proc", 0555);
    g_root_vnode->ops->mkdir(g_root_vnode, "usr", 0755);
    g_root_vnode->ops->mkdir(g_root_vnode, "mnt", 0755);

    struct vnode *usr_vn;
    if (vfs_lookup("/usr", curproc->p_cwd, &usr_vn) == 0) {
        usr_vn->ops->mkdir(usr_vn, "share", 0755);
        struct vnode *share_vn;
        if (vfs_lookup("share", usr_vn, &share_vn) == 0) {
            share_vn->ops->mkdir(share_vn, "games", 0755);
            struct vnode *games_vn;
            if (vfs_lookup("games", share_vn, &games_vn) == 0) {
                games_vn->ops->mkdir(games_vn, "doom", 0755);
                vput(games_vn);
            }
            vput(share_vn);
        }
        vput(usr_vn);
    }

    struct vnode *dev_vn;
    int err_lookup = vfs_lookup("/dev", curproc->p_cwd, &dev_vn);
    if (err_lookup == 0) {
        struct vnode *null_vn;
        struct vnode *tty_vn;
        struct vnode *fb0_vn;
        struct vnode *kbd_vn;
        dev_vn->ops->create(dev_vn, "null", S_IFCHR | 0666, &null_vn);
        dev_vn->ops->create(dev_vn, "tty", S_IFCHR | 0666, &tty_vn);
        dev_vn->ops->create(dev_vn, "fb0", S_IFCHR | 0666, &fb0_vn);
        dev_vn->ops->create(dev_vn, "kbd", S_IFCHR | 0666, &kbd_vn);
        vput(dev_vn);
        if (null_vn) vput(null_vn);
        if (tty_vn) vput(tty_vn);
        if (fb0_vn) vput(fb0_vn);
        if (kbd_vn) vput(kbd_vn);
    }

    if (g_boot_info.initrd.virt_base) {
        vfs_load_initrd(g_boot_info.initrd.virt_base, g_boot_info.initrd.size);
    }
}

static const char* vfs_next_component(const char *path, char *out_name) {
    while (*path == '/') path++; // 선행 슬래시 무시
    if (!*path) return NULL;

    int i = 0;
    while (*path && *path != '/') {
        if (i < 255) out_name[i++] = *path; // NAME_MAX 대충 255 ㅇㅇ
        path++;
    }
    out_name[i] = '\0';
    return path;
}

int vfs_lookup(const char *path, struct vnode *base, struct vnode **res) {
    if (!path || *path == '\0') return -ENOENT;

    struct vnode *curr = (path[0] == '/') ? g_root_vnode : base;
    if (!curr) return -ENOENT;

    vref(curr);

    char name[NAME_MAX];
    const char *next = path;

    while ((next = vfs_next_component(next, name))) {
        struct vnode *found = NULL;

        if (curr->type != S_IFDIR) {
            vput(curr);
            return -ENOTDIR;
        }

        int err = vfs_cached_lookup(curr, name, &found);
        
        if (err != 0) {
            err = curr->ops->lookup(curr, name, &found);
            if (err < 0) {
                vput(curr);
                if (err == -ENOENT && path[0] == '/' && strncmp(path, "/mnt/", 5) == 0) {
                    return vfs_lookup(path + 4, base, res);
                }
                return err;
            }
            vfs_hash_insert(curr, name, found);
        }

        vput(curr);
        curr = found;
    }

    *res = curr;
    return 0;
}

int vfs_open(const char *path, int flags, mode_t mode, int *fd_out) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    struct vnode *vp = NULL;
    int err = vfs_lookup(path, p->p_cwd, &vp);

    if (err == -ENOENT && (flags & O_CREAT)) {
        char parent_path[PATH_MAX];
        char child_name[NAME_MAX];
        
        const char *last_slash = strrchr(path, '/');
        if (!last_slash) {
            // 상대 경로
            strcpy(parent_path, ".");
            strcpy(child_name, path);
        } else {
            // 절대 경로 or 경로포함
            size_t len = last_slash - path;
            if (len == 0) len = 1; // 루트 "/"
            strncpy(parent_path, path, len);
            parent_path[len] = '\0';
            strcpy(child_name, last_slash + 1);
        }

        struct vnode *dvp = NULL;
        err = vfs_lookup(parent_path, p->p_cwd, &dvp);
        if (err != 0) return err;

        if (dvp->ops->create) {
            err = dvp->ops->create(dvp, child_name, mode, &vp);
            if (err == 0) {
                vfs_hash_insert(dvp, child_name, vp);
            }
        }
        
        vput(dvp); // グッバイ! 君の運命のヒトは僕じゃない
        if (err != 0) return err;
    } else if (err != 0) {
        return err;
    }

    struct file *f = file_alloc();
    if (!f) { vput(vp); return -ENOMEM; }

    f->f_vn = vp;
    f->f_flags = flags;
    f->f_pos = 0;

    int fd = proc_alloc_fd(p, f);
    if (fd < 0) { file_close(f); return fd; }

    *fd_out = fd;
    return 0;
}

int vfs_close(int fd) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    if (fd < 0 || fd >= MAX_FILES || p->p_fd_table[fd] == NULL) {
        return -EBADF;
    }

    struct file *f = p->p_fd_table[fd];
    p->p_fd_table[fd] = NULL;

    file_close(f);
    return 0;
}

int vfs_write(int fd, const void *buf, size_t count) {
    struct proc *p = curproc;
    if (fd < 0 || fd >= MAX_FILES || !p->p_fd_table[fd]) return -EBADF;

    struct file *f = p->p_fd_table[fd];
    if (!f->f_vn->ops->write) return -ENOSYS;

    int n = f->f_vn->ops->write(f->f_vn, buf, count, f->f_pos);
    if (n > 0) f->f_pos += n; // 쓴만큼 포인터 전진
    return n;
}

int vfs_read(int fd, void *buf, size_t count) {
    struct proc *p = curproc;
    if (fd < 0 || fd >= MAX_FILES || !p->p_fd_table[fd]) return -EBADF;

    struct file *f = p->p_fd_table[fd];
    if (!f->f_vn->ops->read) return -ENOSYS;

    int n = f->f_vn->ops->read(f->f_vn, buf, count, f->f_pos);
    if (n > 0) f->f_pos += n; // 읽은만큼 포인터 전진
    return n;
}

int vfs_lseek(int fd, off_t offset, int whence) {
    struct proc *p = curproc;
    if (fd < 0 || fd >= MAX_FILES || !p->p_fd_table[fd]) return -EBADF;

    struct file *f = p->p_fd_table[fd];
    struct vnode *vp = f->f_vn;
    off_t new_pos = f->f_pos;

    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = f->f_pos + offset; break;
        case SEEK_END: {
            struct stat st;
            if (vp->ops->getattr) {
                vp->ops->getattr(vp, &st);
                new_pos = st.st_size + offset;
            } else {
                return -ENOTSUP;
            }
            break;
        }
        default: return -EINVAL;
    }

    if (new_pos < 0) return -EINVAL;
    f->f_pos = new_pos;
    return (int)new_pos;
}

int vfs_readdir(int fd, void *buf, size_t count) {
    struct proc *p = curproc;
    if (fd < 0 || fd >= MAX_FILES || !p->p_fd_table[fd]) return -EBADF;

    struct file *f = p->p_fd_table[fd];
    struct vnode *vp = f->f_vn;

    if (vp->type != S_IFDIR) return -ENOTDIR;
    if (!vp->ops->readdir) return -ENOSYS;

    int n = vp->ops->readdir(vp, buf, count, &f->f_pos);
    
    return n;
}

int vfs_mkdir(const char *path, mode_t mode) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char parent_path[256];
    char child_name[256];
    
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        strcpy(parent_path, ".");
        strcpy(child_name, path);
    } else if (last_slash == path) {
        strcpy(parent_path, "/");
        strcpy(child_name, path + 1);
    } else {
        size_t len = last_slash - path;
        strncpy(parent_path, path, len);
        parent_path[len] = '\0';
        strcpy(child_name, last_slash + 1);
    }

    struct vnode *dvp = NULL;
    int err = vfs_lookup(parent_path, p->p_cwd, &dvp);
    if (err != 0) return err;

    if (dvp->ops->mkdir) {
        err = dvp->ops->mkdir(dvp, child_name, mode);
    } else {
        err = -ENOTSUP;
    }

    vput(dvp);
    return err;
}

subsys_initcall(vfs_init);