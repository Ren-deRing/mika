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
#include <kernel/atomic.h>

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

        dev_vn->ops->mkdir(dev_vn, "dri", 0755);
        struct vnode *dri_vn = NULL;
        if (dev_vn->ops->lookup(dev_vn, "dri", &dri_vn) == 0) {
            struct vnode *card0_vn = NULL;
            dri_vn->ops->create(dri_vn, "card0", S_IFCHR | 0666, &card0_vn);
            vput(dri_vn);
            if (card0_vn) vput(card0_vn);
        }

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

void sanitize_path(const char *src, char *dst, size_t dst_size) {
    if (!src || dst_size == 0) return;

    bool is_absolute = (src[0] == '/');

    const char *stack[64];
    int stack_size = 0;

    char *buf = kmalloc(4096);
    if (!buf) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }
    strncpy(buf, src, 4095);
    buf[4095] = '\0';

    char *token = buf;
    char *next_token;
    while (token && *token) {
        char *slash = NULL;
        for (char *p = token; *p; p++) {
            if (*p == '/') {
                slash = p;
                break;
            }
        }
        if (slash) {
            *slash = '\0';
            next_token = slash + 1;
        } else {
            next_token = NULL;
        }

        if (strcmp(token, "") == 0 || strcmp(token, ".") == 0) {
            // Skip
        } else if (strcmp(token, "..") == 0) {
            if (stack_size > 0 && strcmp(stack[stack_size - 1], "..") != 0) {
                stack_size--;
            } else if (!is_absolute) {
                if (stack_size < 64) {
                    stack[stack_size++] = "..";
                }
            }
        } else {
            if (stack_size < 64) {
                stack[stack_size++] = token;
            }
        }

        token = next_token;
    }

    char *d = dst;
    char *d_end = dst + dst_size - 1;

    if (is_absolute) {
        if (d < d_end) *d++ = '/';
    }

    for (int i = 0; i < stack_size; i++) {
        if (i > 0) {
            if (d < d_end) *d++ = '/';
        }
        size_t len = strlen(stack[i]);
        if (d + len < d_end) {
            memcpy(d, stack[i], len);
            d += len;
        } else {
            break;
        }
    }

    if (d == dst) {
        if (is_absolute) {
            if (d < d_end) *d++ = '/';
        } else {
            if (d < d_end) *d++ = '.';
        }
    }

    *d = '\0';

    kfree(buf);
}

static const char* vfs_next_component(const char *path, char *out_name) {
    while (*path == '/') path++; // 선행 슬래시 무시
    if (!*path) return NULL;

    int i = 0;
    while (*path && *path != '/') {
        if (i < NAME_MAX) out_name[i++] = *path;
        path++;
    }
    out_name[i] = '\0';
    return path;
}

static int resolve_symlink(struct vnode *link_vn, int depth, struct vnode **res);

int vfs_lookup_impl(const char *path, struct vnode *base, int follow_last, int depth, struct vnode **res) {
    if (depth > 8) return -ELOOP;
    if (!path || *path == '\0') return -ENOENT;

    struct vnode *curr = (path[0] == '/') ? g_root_vnode : base;
    if (!curr) return -ENOENT;

    vref(curr);

    char name[NAME_MAX + 1];
    const char *next = path;

    while ((next = vfs_next_component(next, name))) {
        if (curr->type == S_IFLNK) {
            struct vnode *target_vn = NULL;
            int err = resolve_symlink(curr, depth, &target_vn);
            if (err < 0) {
                vput(curr);
                return err;
            }
            vput(curr);
            curr = target_vn;
        }

        if (curr->type != S_IFDIR) {
            vput(curr);
            return -ENOTDIR;
        }

        struct vnode *found = NULL;
        int err = vfs_cached_lookup(curr, name, &found);
        
        if (err != 0) {
            err = curr->ops->lookup(curr, name, &found);
            if (err < 0) {
                vput(curr);
                return err;
            }
            vfs_hash_insert(curr, name, found);
        }

        vput(curr);
        curr = found;
    }

    if (curr->type == S_IFLNK && follow_last) {
        struct vnode *target_vn = NULL;
        int err = resolve_symlink(curr, depth, &target_vn);
        if (err < 0) {
            vput(curr);
            return err;
        }
        vput(curr);
        curr = target_vn;
    }

    *res = curr;
    return 0;
}

static int resolve_symlink(struct vnode *link_vn, int depth, struct vnode **res) {
    char *target_buf = kmalloc(512);
    if (!target_buf) return -ENOMEM;

    if (!link_vn->ops->read) {
        kfree(target_buf);
        return -EINVAL;
    }

    ssize_t n = link_vn->ops->read(link_vn, target_buf, 511, 0);
    if (n < 0) {
        kfree(target_buf);
        return n;
    }
    target_buf[n] = '\0';

    struct vnode *base = (target_buf[0] == '/') ? g_root_vnode : link_vn->v_parent;
    if (!base) {
        base = (curproc && curproc->p_cwd) ? curproc->p_cwd : g_root_vnode;
    }

    int err = vfs_lookup_impl(target_buf, base, 1, depth + 1, res);
    kfree(target_buf);
    return err;
}

int vfs_lookup(const char *path, struct vnode *base, struct vnode **res) {
    return vfs_lookup_impl(path, base, 1, 0, res);
}

int vfs_symlink(const char *target, const char *linkpath) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char clean_linkpath[256];
    sanitize_path(linkpath, clean_linkpath, sizeof(clean_linkpath));

    char parent_path[256];
    char child_name[256];
    
    const char *last_slash = strrchr(clean_linkpath, '/');
    if (!last_slash) {
        strcpy(parent_path, ".");
        strcpy(child_name, clean_linkpath);
    } else if (last_slash == clean_linkpath) {
        strcpy(parent_path, "/");
        strcpy(child_name, clean_linkpath + 1);
    } else {
        size_t len = last_slash - clean_linkpath;
        strncpy(parent_path, clean_linkpath, len);
        parent_path[len] = '\0';
        strcpy(child_name, last_slash + 1);
    }

    struct vnode *dvp = NULL;
    int err = vfs_lookup(parent_path, p->p_cwd, &dvp);
    if (err != 0) return err;

    struct vnode *vp = NULL;
    if (dvp->ops->create) {
        err = dvp->ops->create(dvp, child_name, S_IFLNK | 0777, &vp);
        if (err == 0) {
            vfs_hash_insert(dvp, child_name, vp);
            if (vp->ops->write) {
                ssize_t n = vp->ops->write(vp, target, strlen(target), 0);
                if (n < 0) {
                    err = (int)n;
                }
            } else {
                err = -ENOTSUP;
            }
            vput(vp);
        }
    } else {
        err = -ENOTSUP;
    }

    vput(dvp);
    return err;
}

int vfs_open(const char *path, int flags, mode_t mode, int *fd_out) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char clean_path[256];
    sanitize_path(path, clean_path, sizeof(clean_path));

    struct vnode *vp = NULL;
    int err = vfs_lookup(clean_path, p->p_cwd, &vp);

    if (err == -ENOENT && (flags & O_CREAT)) {
        char parent_path[256];
        char child_name[NAME_MAX];
        
        const char *last_slash = strrchr(clean_path, '/');
        if (!last_slash) {
            // 상대 경로
            strcpy(parent_path, ".");
            strcpy(child_name, clean_path);
        } else {
            // 절대 경로 or 경로포함
            size_t len = last_slash - clean_path;
            if (len == 0) len = 1; // 루트 "/"
            strncpy(parent_path, clean_path, len);
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
    file_close(f);

    *fd_out = fd;
    return 0;
}

int vfs_close(int fd) {
    struct proc *p = curproc;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;

    uint64_t flags = spin_lock_irqsave(&p->p_lock);
    struct file *f = p->p_fd_table[fd];
    if (!f) { spin_unlock_irqrestore(&p->p_lock, flags); return -EBADF; }
    p->p_fd_table[fd] = NULL;
    spin_unlock_irqrestore(&p->p_lock, flags);

    file_close(f);
    return 0;
}

int vfs_write(int fd, const void *buf, size_t count) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    if (!f->f_vn || !f->f_vn->ops || !f->f_vn->ops->write) {
        fdput(f); return -ENOSYS;
    }

    int n = f->f_vn->ops->write(f->f_vn, buf, count, f->f_pos);
    if (n > 0) f->f_pos += n;
    fdput(f);
    return n;
}

int vfs_read(int fd, void *buf, size_t count) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    if (!f->f_vn || !f->f_vn->ops || !f->f_vn->ops->read) {
        fdput(f); return -ENOSYS;
    }

    int n = f->f_vn->ops->read(f->f_vn, buf, count, f->f_pos);
    if (n > 0) f->f_pos += n;
    fdput(f);
    return n;
}

int vfs_lseek(int fd, off_t offset, int whence) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    if (!f->f_vn || !f->f_vn->ops) { fdput(f); return -ENOSYS; }

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
                break;
            }
            fdput(f);
            return -ENOTSUP;
        }
        default: fdput(f); return -EINVAL;
    }

    if (new_pos < 0) { fdput(f); return -EINVAL; }

    f->f_pos = new_pos;
    fdput(f);
    return (int)new_pos;
}

int vfs_readdir(int fd, void *buf, size_t count) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;

    if (!f->f_vn || !f->f_vn->ops) { fdput(f); return -ENOSYS; }

    struct vnode *vp = f->f_vn;

    if (vp->type != S_IFDIR) { fdput(f); return -ENOTDIR; }
    if (!vp->ops->readdir) { fdput(f); return -ENOSYS; }

    int n = vp->ops->readdir(vp, buf, count, &f->f_pos);
    fdput(f);
    return n;
}

int vfs_mkdir(const char *path, mode_t mode) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char clean_path[256];
    sanitize_path(path, clean_path, sizeof(clean_path));

    char parent_path[256];
    char child_name[256];
    
    const char *last_slash = strrchr(clean_path, '/');
    if (!last_slash) {
        strcpy(parent_path, ".");
        strcpy(child_name, clean_path);
    } else if (last_slash == clean_path) {
        strcpy(parent_path, "/");
        strcpy(child_name, clean_path + 1);
    } else {
        size_t len = last_slash - clean_path;
        strncpy(parent_path, clean_path, len);
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

int vfs_rmdir(const char *path) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char clean_path[256];
    sanitize_path(path, clean_path, sizeof(clean_path));

    char parent_path[256];
    char child_name[256];

    const char *last_slash = strrchr(clean_path, '/');
    if (!last_slash) {
        strcpy(parent_path, ".");
        strcpy(child_name, clean_path);
    } else if (last_slash == clean_path) {
        strcpy(parent_path, "/");
        strcpy(child_name, clean_path + 1);
    } else {
        size_t len = last_slash - clean_path;
        strncpy(parent_path, clean_path, len);
        parent_path[len] = '\0';
        strcpy(child_name, last_slash + 1);
    }

    struct vnode *dvp = NULL;
    int err = vfs_lookup(parent_path, p->p_cwd, &dvp);
    if (err != 0) return err;

    if (dvp->ops->rmdir) {
        err = dvp->ops->rmdir(dvp, child_name);
    } else if (dvp->ops->remove) {
        err = dvp->ops->remove(dvp, child_name);
    } else {
        err = -ENOTSUP;
    }

    vput(dvp);
    return err;
}

int vfs_bind(const char *source, const char *target) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char clean_source[256];
    char clean_target[256];
    sanitize_path(source, clean_source, sizeof(clean_source));
    sanitize_path(target, clean_target, sizeof(clean_target));

    struct vnode *src_vn = NULL;
    int err = vfs_lookup(clean_source, p->p_cwd, &src_vn);
    if (err != 0) return err;

    char parent_path[256];
    char child_name[256];
    
    const char *last_slash = strrchr(clean_target, '/');
    if (!last_slash) {
        strcpy(parent_path, ".");
        strcpy(child_name, clean_target);
    } else if (last_slash == clean_target) {
        strcpy(parent_path, "/");
        strcpy(child_name, clean_target + 1);
    } else {
        size_t len = last_slash - clean_target;
        strncpy(parent_path, clean_target, len);
        parent_path[len] = '\0';
        strcpy(child_name, last_slash + 1);
    }

    struct vnode *parent_vn = NULL;
    err = vfs_lookup(parent_path, p->p_cwd, &parent_vn);
    if (err != 0) {
        vput(src_vn);
        return err;
    }

    if (parent_vn->type != S_IFDIR) {
        vput(src_vn);
        vput(parent_vn);
        return -ENOTDIR;
    }

    struct ramfs_node *parent_node = (struct ramfs_node *)parent_vn->data;
    struct ramfs_entry *entry = kmalloc(sizeof(struct ramfs_entry));
    if (!entry) {
        vput(src_vn);
        vput(parent_vn);
        return -ENOMEM;
    }

    strncpy(entry->name, child_name, 255);
    entry->name[255] = '\0';
    vref(src_vn);
    entry->vn = src_vn;
    entry->next = parent_node->entries;
    parent_node->entries = entry;

    vput(src_vn);
    vput(parent_vn);
    return 0;
}

int vfs_unlink(const char *path) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char clean_path[256];
    sanitize_path(path, clean_path, sizeof(clean_path));

    char parent_path[256];
    char child_name[256];
    
    const char *last_slash = strrchr(clean_path, '/');
    if (!last_slash) {
        strcpy(parent_path, ".");
        strcpy(child_name, clean_path);
    } else if (last_slash == clean_path) {
        strcpy(parent_path, "/");
        strcpy(child_name, clean_path + 1);
    } else {
        size_t len = last_slash - clean_path;
        strncpy(parent_path, clean_path, len);
        parent_path[len] = '\0';
        strcpy(child_name, last_slash + 1);
    }

    struct vnode *dvp = NULL;
    int err = vfs_lookup(parent_path, p->p_cwd, &dvp);
    if (err != 0) return err;

    if (dvp->ops->remove) {
        err = dvp->ops->remove(dvp, child_name);
    } else {
        err = -ENOTSUP;
    }

    vput(dvp);
    return err;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!curthread || !curthread->t_proc) return -ESRCH;
    struct proc *p = curthread->t_proc;

    char clean_old[256];
    char clean_new[256];
    sanitize_path(oldpath, clean_old, sizeof(clean_old));
    sanitize_path(newpath, clean_new, sizeof(clean_new));

    char old_parent[256];
    char old_child[256];
    const char *last_slash_old = strrchr(clean_old, '/');
    if (!last_slash_old) {
        strcpy(old_parent, ".");
        strcpy(old_child, clean_old);
    } else if (last_slash_old == clean_old) {
        strcpy(old_parent, "/");
        strcpy(old_child, clean_old + 1);
    } else {
        size_t len = last_slash_old - clean_old;
        strncpy(old_parent, clean_old, len);
        old_parent[len] = '\0';
        strcpy(old_child, last_slash_old + 1);
    }

    char new_parent[256];
    char new_child[256];
    const char *last_slash_new = strrchr(clean_new, '/');
    if (!last_slash_new) {
        strcpy(new_parent, ".");
        strcpy(new_child, clean_new);
    } else if (last_slash_new == clean_new) {
        strcpy(new_parent, "/");
        strcpy(new_child, clean_new + 1);
    } else {
        size_t len = last_slash_new - clean_new;
        strncpy(new_parent, clean_new, len);
        new_parent[len] = '\0';
        strcpy(new_child, last_slash_new + 1);
    }

    struct vnode *sdvp = NULL;
    int err = vfs_lookup(old_parent, p->p_cwd, &sdvp);
    if (err != 0) return err;

    struct vnode *tdvp = NULL;
    err = vfs_lookup(new_parent, p->p_cwd, &tdvp);
    if (err != 0) {
        vput(sdvp);
        return err;
    }

    if (sdvp->ops->rename) {
        err = sdvp->ops->rename(sdvp, old_child, tdvp, new_child);
    } else {
        err = -ENOTSUP;
    }

    vput(sdvp);
    vput(tdvp);
    return err;
}

struct file *fdget(int fd) {
    if (fd < 0 || fd >= MAX_FILES) return NULL;
    struct proc *p = curproc;
    rcu_read_lock();
    struct file *f = __atomic_load_n(&p->p_fd_table[fd], __ATOMIC_ACQUIRE);
    if (f && !atomic_inc_not_zero(&f->f_refcnt))
        f = NULL;
    rcu_read_unlock();
    return f;
}

void fdput(struct file *f) {
    if (f) file_close(f);
}

subsys_initcall(vfs_init);