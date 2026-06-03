#pragma once
#include <kernel/fs/vnode.h>

struct ramfs_entry {
    char name[256];
    struct vnode *vn;
    struct ramfs_entry *next;
};

struct ramfs_node {
    char *buffer;
    size_t size;
    int is_static_buf;
    
    mode_t mode;
    uid_t uid;
    gid_t gid;
    struct timespec atime, mtime, ctime;
    int nlink;

    int unlinked;
    struct ramfs_entry *entries;
};
extern struct vnode_ops ramfs_ops;

void ramfs_init(void);
struct vnode* ramfs_create_vnode(uint32_t type);