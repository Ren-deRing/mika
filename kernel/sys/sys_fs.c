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
#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>
#include <kernel/fs/mount.h>
#include <uapi/sys/stat.h>
#include <uapi/fcntl.h>
#include <string.h>

void fill_rdev(struct vnode *vn, struct stat *st) {
    if (vn && S_ISCHR(st->st_mode)) {
        st->st_rdev = vn->rdev;
    }
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

int64_t sys_mkdir(const char *user_path, int mode) {
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    int err = vfs_mkdir(kpath, (mode_t)mode);
    return (int64_t)err;
}

extern int vfs_rmdir(const char *path);

int64_t sys_rmdir(const char *user_path) {
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    int err = vfs_rmdir(kpath);
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

    int err = vfs_mount(source, target, fstype, NULL);
    return (int64_t)err;
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

    int fd_out = -1;
    int open_flags = O_RDWR | O_CREAT;
    int err = vfs_open(path, open_flags, 0600, &fd_out);
    if (err < 0) {
        return (int64_t)err;
    }

    return (int64_t)fd_out;
}

int64_t sys_pivot_root(const char *user_new_root, const char *user_put_old) {
    char knew_root[256], kput_old[256];
    if (copy_str_from_user(knew_root, user_new_root, sizeof(knew_root)) < 0)
        return -EFAULT;
    if (copy_str_from_user(kput_old, user_put_old, sizeof(kput_old)) < 0)
        return -EFAULT;

    struct vnode *new_root_vn = NULL;
    int err = vfs_lookup(knew_root, NULL, &new_root_vn);
    if (err < 0) return err;
    if (new_root_vn->type != S_IFDIR) { vput(new_root_vn); return -ENOTDIR; }

    struct mount *mnt = NULL;
    spin_lock(&mount_lock);
    struct mount *pos;
    list_for_each_entry(pos, &mount_list, mnt_list) {
        if (pos->mnt_root == new_root_vn && (pos->mnt_flags & MOUNTED)) {
            mnt = pos;
            break;
        }
    }
    spin_unlock(&mount_lock);
    if (!mnt) { vput(new_root_vn); return -EINVAL; }

    char leaf[NAME_MAX + 1];
    struct vnode *put_old_parent = NULL;
    size_t leaf_len = 0;
    do {
        const char *slash = kput_old;
        const char *last_slash = NULL;
        while (*slash) {
            if (*slash == '/') last_slash = slash;
            slash++;
        }
        if (!last_slash || last_slash == kput_old) { vput(new_root_vn); return -EINVAL; }

        char parent_path[256];
        size_t parent_len = last_slash - kput_old;
        if (parent_len >= sizeof(parent_path)) { vput(new_root_vn); return -ENAMETOOLONG; }
        memcpy(parent_path, kput_old, parent_len);
        parent_path[parent_len] = '\0';

        leaf_len = strlen(last_slash + 1);
        if (leaf_len == 0 || leaf_len > NAME_MAX) { vput(new_root_vn); return -EINVAL; }
        memcpy(leaf, last_slash + 1, leaf_len + 1);

        struct vnode *walk = mnt->mnt_root;
        vref(walk);
        char component[NAME_MAX + 1];
        const char *p = parent_path;
        while (*p == '/') p++;
        if (*p) {
            while (1) {
                const char *next_slash = NULL;
                {
                    const char *t = p;
                    while (*t) { if (*t == '/') { next_slash = t; break; } t++; }
                }
                size_t clen = next_slash ? (size_t)(next_slash - p) : strlen(p);
                if (clen == 0 || clen > NAME_MAX) { vput(walk); vput(new_root_vn); return -ENOENT; }
                memcpy(component, p, clen);
                component[clen] = '\0';

                struct vnode *child = NULL;
                int e = walk->ops->lookup(walk, component, &child);
                if (e < 0) {
                    e = walk->ops->mkdir(walk, component, 0755);
                    if (e < 0) { vput(walk); vput(new_root_vn); return e; }
                    e = walk->ops->lookup(walk, component, &child);
                }
                if (e < 0) { vput(walk); vput(new_root_vn); return e; }
                vput(walk);
                walk = child;
                if (!next_slash) break;
                p = next_slash + 1;
            }
        }
        put_old_parent = walk;
    } while (0);

    struct vnode *put_old_vn = NULL;
    int e = put_old_parent->ops->lookup(put_old_parent, leaf, &put_old_vn);
    if (e < 0) {
        e = put_old_parent->ops->mkdir(put_old_parent, leaf, 0755);
        if (e < 0) { vput(put_old_parent); vput(new_root_vn); return e; }
        e = put_old_parent->ops->lookup(put_old_parent, leaf, &put_old_vn);
    }
    if (e < 0) { vput(put_old_parent); vput(new_root_vn); return e; }
    vput(put_old_parent);

    struct mount *old_root_mnt = g_root_vnode->mnt;
    if (!old_root_mnt || !(old_root_mnt->mnt_flags & MOUNTED)) {
        vput(new_root_vn); vput(put_old_vn); return -EINVAL;
    }

    // 새 root Detach
    mnt->mnt_mountpoint->mnt = NULL;
    list_del(&mnt->mnt_list);

    // 기존 root Detach
    g_root_vnode->mnt = NULL;
    list_del(&old_root_mnt->mnt_list);

    // 새 root를 root로 Mount
    mnt->mnt_mountpoint = g_root_vnode;
    g_root_vnode->mnt = mnt;
    spin_lock(&mount_lock);
    list_add_tail(&mnt->mnt_list, &mount_list);
    spin_unlock(&mount_lock);

    // 기존 root를 새로운 곳으로 Mount
    old_root_mnt->mnt_mountpoint = put_old_vn;
    put_old_vn->mnt = old_root_mnt;
    spin_lock(&mount_lock);
    list_add_tail(&old_root_mnt->mnt_list, &mount_list);
    spin_unlock(&mount_lock);

    vput(new_root_vn);
    vput(put_old_vn);

    return 0;
}

int64_t sys_mknod(const char *user_path, mode_t mode, uint64_t dev) {
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, sizeof(kpath)) < 0)
        return -EFAULT;

    if ((mode & S_IFMT) != S_IFCHR && (mode & S_IFMT) != S_IFBLK)
        return -EINVAL;

    char parent_path[256];
    char name[NAME_MAX + 1];
    const char *slash = kpath;
    const char *last_slash = NULL;
    while (*slash) { if (*slash == '/') last_slash = slash; slash++; }
    if (!last_slash || last_slash == kpath) return -EINVAL;
    size_t plen = last_slash - kpath;
    if (plen >= sizeof(parent_path)) return -ENAMETOOLONG;
    memcpy(parent_path, kpath, plen);
    parent_path[plen] = '\0';
    size_t nlen = strlen(last_slash + 1);
    if (nlen == 0 || nlen > NAME_MAX) return -EINVAL;
    memcpy(name, last_slash + 1, nlen + 1);

    struct vnode *dvp = NULL;
    int err = vfs_lookup(parent_path, NULL, &dvp);
    if (err < 0) return err;
    if (dvp->type != S_IFDIR) { vput(dvp); return -ENOTDIR; }

    struct vnode *vp = NULL;
    err = dvp->ops->create(dvp, name, mode, &vp);
    if (err < 0) { vput(dvp); return err; }

    vp->rdev = (uint32_t)dev;
    vput(vp);
    vput(dvp);
    return 0;
}
