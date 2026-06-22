#pragma once

#include <uapi/sys/stat.h>

#include <kernel/list.h>
#include <kernel/lock.h>

#define VNODE_HASH_SIZE 512

struct vnode;

struct vnode_ops {
    int (*lookup)(struct vnode *dvp, const char *name, struct vnode **vpp);
    int (*create)(struct vnode *dvp, const char *name, mode_t mode, struct vnode **vpp);
    int (*mkdir)(struct vnode *dvp, const char *name, mode_t mode);
    int (*open)(struct vnode *vp, int flags);
    int (*close)(struct vnode *vp);
    ssize_t (*read)(struct vnode *vp, void *buf, size_t n, off_t off);
    ssize_t (*write)(struct vnode *vp, const void *buf, size_t n, off_t off);
    int (*ioctl)(struct vnode *vp, uint64_t request, void *arg);
    int (*getattr)(struct vnode *vp, struct stat *st);
    int (*setattr)(struct vnode *vp, struct stat *st);
    int (*readdir)(struct vnode *vp, void *dirent_buf, size_t count, off_t *off);
    int (*inactive)(struct vnode *vp);
    int (*remove)(struct vnode *dvp, const char *name);
    int (*rmdir)(struct vnode *dvp, const char *name);
    int (*rename)(struct vnode *sdvp, const char *sname, struct vnode *tdvp, const char *dname);
};

struct vnode {
    rwlock_t          rwlock;       /* data 보호 */
    mutex_t           io_mutex;     /* file I/O mutex (read/write) */
    uint32_t          ref_count;
    uint32_t          type;
    struct vnode_ops *ops;
    struct mount     *mnt;
    void             *data;
    spinlock_t        lock;         /* 메타데이터용 (v_reclaimable etc) */

    struct list_node  v_all;
    struct list_node  v_freelist;
    struct list_node  v_hash;

    struct vnode *v_parent;
    char v_name[32];
    int  v_reclaimable;
};

static inline void vref(struct vnode *vn) {
    if (vn) __atomic_fetch_add(&vn->ref_count, 1, __ATOMIC_SEQ_CST);
}

void vnode_init(void);

struct vnode* vnode_alloc(uint32_t type, struct vnode_ops *ops);
int vfs_cached_lookup(struct vnode *dvp, const char *name, struct vnode **vpp);
void vfs_hash_insert(struct vnode *dvp, const char *name, struct vnode *vp);
void vput(struct vnode *vn);
int vget(struct vnode *vp);