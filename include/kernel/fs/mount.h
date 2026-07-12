#pragma once

#include <kernel/list.h>
#include <kernel/fs/vnode.h>

struct vnode;

typedef struct vnode *(*fs_mount_t)(struct vnode *dev_vp, void *data);

struct file_system_type {
    const char *name;
    fs_mount_t  mount;
    struct list_node fs_list;
    int fs_flags;
};

struct mount {
    struct vnode *mnt_root;
    struct vnode *mnt_mountpoint;
    struct file_system_type *mnt_fs;
    void *mnt_data;
    struct list_node mnt_list;
    int mnt_flags;
};

#define MOUNTED (1 << 0)

extern struct list_node mount_list;
extern spinlock_t mount_lock;

void register_filesystem(struct file_system_type *fst);
void unregister_filesystem(struct file_system_type *fst);
struct file_system_type *get_fs_type(const char *name);

int vfs_mount(const char *dev_path, const char *target_path,
              const char *fstype, void *data);
int vfs_umount(const char *target_path);

struct vnode *mount_point_follow(struct vnode *vp);
int mount_path_resolve(struct vnode **vpp);
