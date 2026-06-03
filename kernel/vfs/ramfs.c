#include <kernel/fs/vnode.h>
#include <kernel/fs/ramfs.h>
#include <kernel/kmem.h>
#include <string.h>

#include <uapi/errno.h>
#include <uapi/sys/stat.h>
#include <uapi/sys/dirent.h>

struct vnode_ops ramfs_ops;

struct vnode* ramfs_create_vnode(uint32_t type) {
    struct vnode *vn = vnode_alloc(type, &ramfs_ops);
    if (!vn) return NULL;

    struct ramfs_node *node = kmalloc(sizeof(struct ramfs_node));
    if (!node) {
        kfree(vn);
        return NULL;
    }
    memset(node, 0, sizeof(struct ramfs_node));
    
    vn->data = node;
    return vn;
}

int ramfs_lookup(struct vnode *dvp, const char *name, struct vnode **vpp) {
    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;
    
    // TODO: Move to VFS
    if (strcmp(name, ".") == 0) {
        vref(dvp);
        *vpp = dvp;
        return 0;
    }

    struct ramfs_entry *entry = dnode->entries;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            vref(entry->vn);
            *vpp = entry->vn;
            return 0;
        }
        entry = entry->next;
    }

    return -ENOENT;
}

int ramfs_mkdir(struct vnode *dvp, const char *name, mode_t mode) {
    (void)mode; // TODO

    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;

    // ENAMETOOLONG Check
    if (strlen(name) >= 256) return -ENAMETOOLONG;

    // EEXIST Check
    struct vnode *existing;
    if (ramfs_lookup(dvp, name, &existing) == 0) {
        vput(existing); 
        return -EEXIST;
    }

    struct vnode *nvp = ramfs_create_vnode(S_IFDIR);
    if (!nvp) return -ENOMEM;

    struct ramfs_entry *entry = kmalloc(sizeof(struct ramfs_entry));
    if (!entry) {
        vput(nvp);
        return -ENOMEM;
    }

    strncpy(entry->name, name, 255);
    entry->vn = nvp;
    
    entry->next = dnode->entries;
    dnode->entries = entry;

    return 0;
}

int ramfs_create(struct vnode *dvp, const char *name, mode_t mode, struct vnode **vpp) {
    (void)mode;

    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;

    // ENAMETOOLONG Check
    if (strlen(name) >= 256) return -ENAMETOOLONG;

    // EEXIST Check
    struct vnode *exist;
    if (ramfs_lookup(dvp, name, &exist) == 0) {
        vput(exist);
        return -EEXIST;
    }

    // vnode 생성
    struct vnode *nvp = ramfs_create_vnode(S_IFREG);
    if (!nvp) return -ENOMEM;

    struct ramfs_entry *entry = kmalloc(sizeof(struct ramfs_entry));
    if (!entry) {
        vput(nvp);
        return -ENOMEM;
    }

    strncpy(entry->name, name, 255);
    entry->vn = nvp;
    entry->next = dnode->entries;
    dnode->entries = entry;

    vref(nvp);
    *vpp = nvp;

    return 0;
}

int ramfs_inactive(struct vnode *vp) {
    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    if (!node) return 0;

    if (node->unlinked) {
        if (vp->type == S_IFDIR) {
            struct ramfs_entry *curr = node->entries;
            while (curr) {
                struct ramfs_entry *next = curr->next;
                kfree(curr);
                curr = next;
            }
        }
        if (node->buffer && !node->is_static_buf) kfree(node->buffer);
        kfree(node);
        vp->data = NULL;
    }

    return 0;
}

ssize_t ramfs_write(struct vnode *vp, const void *buf, size_t count, off_t off) {
    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    if (vp->type != S_IFREG) return -EISDIR;

    if (node->is_static_buf) {
        char *new_buf = kmalloc(node->size);
        if (!new_buf) return -ENOMEM;
        if (node->buffer && node->size > 0) {
            memcpy(new_buf, node->buffer, node->size);
        }
        node->buffer = new_buf;
        node->is_static_buf = 0;
    }

    if (off + count > node->size) {
        size_t new_size = off + count;

        char *new_buf = krealloc(node->buffer, new_size);
        if (!new_buf) return -ENOMEM;

        node->buffer = new_buf;
        node->size = new_size;
    }

    memcpy(node->buffer + off, buf, count);
    return (ssize_t)count;
}

ssize_t ramfs_read(struct vnode *vp, void *buf, size_t count, off_t off) {
    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    if (vp->type != S_IFREG) return -EISDIR;

    if (off >= (off_t)node->size) return 0; // EOF
    if (off + count > node->size) {
        count = node->size - off;
    }

    memcpy(buf, node->buffer + off, count);
    return (ssize_t)count;
}

int ramfs_remove(struct vnode *dvp, const char *name) {
    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;
    struct ramfs_entry **prev = &dnode->entries;
    struct ramfs_entry *curr = dnode->entries;

    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            *prev = curr->next;
            
            struct ramfs_node *fnode = (struct ramfs_node *)curr->vn->data;
            fnode->unlinked = 1;

            vput(curr->vn);
            kfree(curr);
            return 0;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    return -ENOENT;
}

int ramfs_readdir(struct vnode *vp, void *dirent_buf, size_t count, off_t *off) {
    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    struct ramfs_entry *curr = node->entries;
    
    for (off_t i = 0; i < *off && curr; i++) {
        curr = curr->next;
    }

    if (!curr) return 0; // 읽을 게 없음

    struct dirent *de = (struct dirent *)dirent_buf;
    size_t name_len = strlen(curr->name);
    
    uint16_t reclen = DIRENT_ALIGN(offsetof(struct dirent, d_name) + name_len + 1);
    
    if (reclen > count) return -EINVAL;

    de->d_ino = (uintptr_t)curr->vn; // TODO: vnode = inode
    de->d_reclen = reclen;
    de->d_type = (curr->vn->type == S_IFDIR) ? DT_DIR : DT_REG;
    de->d_off = (*off) + 1;
    strcpy(de->d_name, curr->name);

    (*off)++; 
    return (int)reclen;
}

int ramfs_getattr(struct vnode *vp, struct stat *st) {
    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    memset(st, 0, sizeof(struct stat));
    
    st->st_size = node->size;
    st->st_mode = vp->type;
    // TODO
    
    return 0;
}

struct vnode_ops ramfs_ops = {
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .mkdir    = ramfs_mkdir,
    .inactive = ramfs_inactive,
    .write    = ramfs_write,
    .read     = ramfs_read,
    .remove   = ramfs_remove,
    .getattr  = ramfs_getattr,
    .readdir  = ramfs_readdir,
};