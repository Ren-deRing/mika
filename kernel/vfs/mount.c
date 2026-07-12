#include <kernel/fs/vfs.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/vnode.h>
#include <kernel/kmem.h>
#include <kernel/printf.h>
#include <kernel/init.h>

#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <string.h>

static struct list_node fs_type_list = LIST_HEAD_INIT(fs_type_list);
struct list_node mount_list  = LIST_HEAD_INIT(mount_list);
spinlock_t mount_lock;

static int mount_inited = 0;

void register_filesystem(struct file_system_type *fst) {
    if (!fst || !fst->name || !fst->mount) return;
    spin_lock(&mount_lock);
    struct file_system_type *pos;
    list_for_each_entry(pos, &fs_type_list, fs_list) {
        if (strcmp(pos->name, fst->name) == 0) {
            spin_unlock(&mount_lock);
            return;
        }
    }
    list_add_tail(&fst->fs_list, &fs_type_list);
    spin_unlock(&mount_lock);
}

void unregister_filesystem(struct file_system_type *fst) {
    if (!fst) return;
    spin_lock(&mount_lock);
    list_del(&fst->fs_list);
    spin_unlock(&mount_lock);
}

struct file_system_type *get_fs_type(const char *name) {
    struct file_system_type *pos;
    spin_lock(&mount_lock);
    list_for_each_entry(pos, &fs_type_list, fs_list) {
        if (strcmp(pos->name, name) == 0) {
            spin_unlock(&mount_lock);
            return pos;
        }
    }
    spin_unlock(&mount_lock);
    return NULL;
}

int vfs_mount(const char *dev_path, const char *target_path,
              const char *fstype, void *data) {
    if (!dev_path || !target_path || !fstype) return -EINVAL;

    struct file_system_type *fst = get_fs_type(fstype);
    if (!fst) {
        dprintf("[MOUNT] Unknown filesystem type: %s\n", fstype);
        return -ENODEV;
    }

    struct vnode *target_vn = NULL;
    int err = vfs_lookup(target_path, NULL, &target_vn);
    if (err != 0) {
        dprintf("[MOUNT] Target %s not found: %d\n", target_path, err);
        return err;
    }

    if (target_vn->type != S_IFDIR) {
        vput(target_vn);
        return -ENOTDIR;
    }

    struct vnode *dev_vn = NULL;
    if (dev_path && dev_path[0]) {
        err = vfs_lookup(dev_path, NULL, &dev_vn);
        if (err != 0) {
            vput(target_vn);
            return err;
        }
    }

    struct vnode *root_vn = fst->mount(dev_vn, data);
    if (dev_vn) vput(dev_vn);
    if (!root_vn) {
        vput(target_vn);
        return -EIO;
    }

    struct mount *mnt = kmalloc(sizeof(struct mount));
    if (!mnt) {
        vput(root_vn);
        vput(target_vn);
        return -ENOMEM;
    }

    __builtin_memset(mnt, 0, sizeof(*mnt));
    mnt->mnt_root = root_vn;
    mnt->mnt_fs = fst;
    mnt->mnt_data = data;
    mnt->mnt_mountpoint = target_vn;
    mnt->mnt_flags = MOUNTED;

    root_vn->mnt = NULL;
    target_vn->mnt = mnt;

    spin_lock(&mount_lock);
    list_add_tail(&mnt->mnt_list, &mount_list);
    spin_unlock(&mount_lock);

    return 0;
}

int vfs_umount(const char *target_path) {
    if (!target_path) return -EINVAL;

    struct vnode *target_vn = NULL;
    int err = vfs_lookup(target_path, NULL, &target_vn);
    if (err != 0) return err;

    spin_lock(&mount_lock);
    struct list_node *node = mount_list.next;
    while (node != &mount_list) {
        struct mount *pos = list_entry(node, struct mount, mnt_list);
        node = node->next;
        if (pos->mnt_mountpoint == target_vn) {
            list_del(&pos->mnt_list);
            target_vn->mnt = NULL;
            spin_unlock(&mount_lock);
            vput(pos->mnt_root);
            vput(target_vn);
            kfree(pos);
            return 0;
        }
    }
    spin_unlock(&mount_lock);
    vput(target_vn);
    return -EINVAL;
}

struct vnode *mount_point_follow(struct vnode *vp) {
    if (!vp) return NULL;
    if (vp->mnt && (vp->mnt->mnt_flags & MOUNTED)) {
        return vp->mnt->mnt_root;
    }
    return vp;
}

static void mount_init(void) {
    spin_lock_init(&mount_lock);
    mount_inited = 1;
}

subsys_initcall(mount_init, PRIO_FIRST);
