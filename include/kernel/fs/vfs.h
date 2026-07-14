#pragma once

#include <kernel/fs/vnode.h>
#include <kernel/fs/file.h>

extern struct vnode *g_root_vnode;

void vfs_init(void);
void vfs_load_initrd(uintptr_t addr, uint64_t size);

struct file *fdget(int fd);
void fdput(struct file *f);

int vfs_lookup(const char *path, struct vnode *base, struct vnode **vpp);
int vfs_lookup_impl(const char *path, struct vnode *base, int follow_last, int depth, struct vnode **vpp);
int vfs_symlink(const char *target, const char *linkpath);
void sanitize_path(const char *src, char *dst, size_t dst_size);
int vfs_open(const char *path, int flags, mode_t mode, int *fd);
int vfs_close(int fd);
int vfs_read(int fd, void *buf, size_t n);
int vfs_write(int fd, const void *buf, size_t n);
int vfs_lseek(int fd, off_t offset, int whence);
int vfs_readdir(int fd, void *buf, size_t count);
int vfs_mkdir(const char *path, mode_t mode);
int vfs_bind(const char *source, const char *target);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);

int vfs_permission(mode_t want, uid_t uid, gid_t gid, mode_t perms);
int vfs_may_open(struct vnode *vp, int flags);