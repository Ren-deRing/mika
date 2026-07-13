#include <kernel/fs/vnode.h>
#include <kernel/fs/ramfs.h>
#include <kernel/kmem.h>
#include <kernel/lock.h>
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

    node->mode = (type == S_IFDIR) ? 0755 : 0644;
    node->uid = 0;
    node->gid = 0;
    node->nlink = (type == S_IFDIR) ? 2 : 1;

    vn->data = node;
    vn->v_reclaimable = 0;
    return vn;
}

int ramfs_lookup(struct vnode *dvp, const char *name, struct vnode **vpp) {
    read_lock(&dvp->rwlock);

    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;

    if (strcmp(name, ".") == 0) {
        vref(dvp);
        *vpp = dvp;
        read_unlock(&dvp->rwlock);
        return 0;
    }

    struct ramfs_entry *entry = dnode->entries;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            vref(entry->vn);
            *vpp = entry->vn;
            read_unlock(&dvp->rwlock);
            return 0;
        }
        entry = entry->next;
    }

    read_unlock(&dvp->rwlock);
    return -ENOENT;
}

static int ramfs_lookup_locked(struct vnode *dvp, const char *name, struct vnode **vpp) {
    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;

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
    if (strlen(name) >= 256) return -ENAMETOOLONG;

    write_lock(&dvp->rwlock);

    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;

    struct vnode *existing;
    if (ramfs_lookup_locked(dvp, name, &existing) == 0) {
        vput(existing);
        write_unlock(&dvp->rwlock);
        return -EEXIST;
    }

    struct vnode *nvp = ramfs_create_vnode(S_IFDIR);
    if (!nvp) {
        write_unlock(&dvp->rwlock);
        return -ENOMEM;
    }

    struct ramfs_node *node = (struct ramfs_node *)nvp->data;
    node->mode = mode & 07777;
    node->uid = 0;
    node->gid = 0;
    node->nlink = 2;

    struct ramfs_entry *entry = kmalloc(sizeof(struct ramfs_entry));
    if (!entry) {
        vput(nvp);
        write_unlock(&dvp->rwlock);
        return -ENOMEM;
    }

    strncpy(entry->name, name, 255);
    entry->vn = nvp;

    entry->next = dnode->entries;
    dnode->entries = entry;

    write_unlock(&dvp->rwlock);
    return 0;
}

int ramfs_create(struct vnode *dvp, const char *name, mode_t mode, struct vnode **vpp) {
    if (strlen(name) >= 256) return -ENAMETOOLONG;

    write_lock(&dvp->rwlock);

    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;

    struct vnode *exist;
    if (ramfs_lookup_locked(dvp, name, &exist) == 0) {
        vput(exist);
        write_unlock(&dvp->rwlock);
        return -EEXIST;
    }

    uint32_t type = mode & S_IFMT;
    if (!type) type = S_IFREG;
    struct vnode *nvp = ramfs_create_vnode(type);
    if (!nvp) {
        write_unlock(&dvp->rwlock);
        return -ENOMEM;
    }

    struct ramfs_node *node = (struct ramfs_node *)nvp->data;
    node->mode = mode & 07777;
    node->uid = 0;
    node->gid = 0;
    node->nlink = 1;

    struct ramfs_entry *entry = kmalloc(sizeof(struct ramfs_entry));
    if (!entry) {
        vput(nvp);
        write_unlock(&dvp->rwlock);
        return -ENOMEM;
    }

    strncpy(entry->name, name, 255);
    entry->vn = nvp;
    entry->next = dnode->entries;
    dnode->entries = entry;

    node->my_entry = entry;
    node->parent_dv = dvp;

    vref(nvp);
    *vpp = nvp;

    write_unlock(&dvp->rwlock);
    return 0;
}

int ramfs_inactive(struct vnode *vp) {
    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    if (!node) return 0;

    if (node->unlinked && vp->type == S_IFDIR) {
        struct ramfs_entry *curr = node->entries;
        while (curr) {
            struct ramfs_entry *next = curr->next;
            kfree(curr);
            curr = next;
        }
    }

    if (!node->is_static_buf && node->blocks) {
        size_t num_blocks = (node->size + 4095) / 4096;
        for (size_t i = 0; i < num_blocks; i++) {
            if (node->blocks[i]) kfree(node->blocks[i]);
        }
        kfree(node->blocks);
    }

    kfree(node);
    vp->data = NULL;
    return 0;
}

ssize_t ramfs_write(struct vnode *vp, const void *buf, size_t count, off_t off) {
    mutex_lock(&vp->io_mutex);

    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    if (vp->type != S_IFREG && vp->type != S_IFLNK) {
        mutex_unlock(&vp->io_mutex);
        return -EISDIR;
    }

    if (node->is_static_buf) {
        size_t old_size = node->size;
        size_t num_blocks = (old_size + 4095) / 4096;
        char **new_blocks = NULL;

        if (num_blocks > 0) {
            new_blocks = kmalloc(num_blocks * sizeof(char *));
            if (!new_blocks) { mutex_unlock(&vp->io_mutex); return -ENOMEM; }
            memset(new_blocks, 0, num_blocks * sizeof(char *));

            for (size_t i = 0; i < num_blocks; i++) {
                new_blocks[i] = kmalloc(4096);
                if (!new_blocks[i]) {
                    for (size_t j = 0; j < i; j++) kfree(new_blocks[j]);
                    kfree(new_blocks);
                    mutex_unlock(&vp->io_mutex);
                    return -ENOMEM;
                }

                size_t offset = i * 4096;
                size_t to_copy = 4096;
                if (offset + to_copy > old_size) {
                    to_copy = old_size - offset;
                }

                memcpy(new_blocks[i], node->buffer + offset, to_copy);
                if (to_copy < 4096) {
                    memset(new_blocks[i] + to_copy, 0, 4096 - to_copy);
                }
            }
        }

        node->blocks = new_blocks;
        node->is_static_buf = 0;
    }

    if (off + count > node->size) {
        size_t new_size = off + count;
        size_t old_block_count = (node->size + 4095) / 4096;
        size_t new_block_count = (new_size + 4095) / 4096;

        if (new_block_count > old_block_count) {
            char **new_blocks = krealloc(node->blocks, new_block_count * sizeof(char *));
            if (!new_blocks) { mutex_unlock(&vp->io_mutex); return -ENOMEM; }

            for (size_t i = old_block_count; i < new_block_count; i++) {
                new_blocks[i] = NULL;
            }
            node->blocks = new_blocks;
        }
        node->size = new_size;
    }

    const char *src = (const char *)buf;
    size_t bytes_written = 0;

    while (bytes_written < count) {
        off_t current_off = off + bytes_written;
        size_t block_idx = current_off / 4096;
        size_t block_off = current_off % 4096;

        size_t to_copy = 4096 - block_off;
        if (to_copy > (count - bytes_written)) {
            to_copy = count - bytes_written;
        }

        if (!node->blocks[block_idx]) {
            node->blocks[block_idx] = kmalloc(4096);
            if (!node->blocks[block_idx]) { mutex_unlock(&vp->io_mutex); return -ENOMEM; }
            memset(node->blocks[block_idx], 0, 4096);
        }

        memcpy(node->blocks[block_idx] + block_off, src + bytes_written, to_copy);
        bytes_written += to_copy;
    }

    mutex_unlock(&vp->io_mutex);
    return (ssize_t)count;
}

ssize_t ramfs_read(struct vnode *vp, void *buf, size_t count, off_t off) {
    mutex_lock(&vp->io_mutex);

    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    if (vp->type != S_IFREG && vp->type != S_IFLNK) {
        mutex_unlock(&vp->io_mutex);
        return -EISDIR;
    }

    if (off >= (off_t)node->size) { mutex_unlock(&vp->io_mutex); return 0; }
    if (off + count > node->size) {
        count = node->size - off;
    }

    if (count == 0) { mutex_unlock(&vp->io_mutex); return 0; }

    if (node->is_static_buf) {
        memcpy(buf, node->buffer + off, count);
    } else {
        char *dest = (char *)buf;
        size_t bytes_read = 0;

        while (bytes_read < count) {
            off_t current_off = off + bytes_read;
            size_t block_idx = current_off / 4096;
            size_t block_off = current_off % 4096;

            size_t to_copy = 4096 - block_off;
            if (to_copy > (count - bytes_read)) {
                to_copy = count - bytes_read;
            }

            if (node->blocks && node->blocks[block_idx]) {
                memcpy(dest + bytes_read, node->blocks[block_idx] + block_off, to_copy);
            } else {
                memset(dest + bytes_read, 0, to_copy);
            }

            bytes_read += to_copy;
        }
    }

    mutex_unlock(&vp->io_mutex);
    return (ssize_t)count;
}

int ramfs_remove(struct vnode *dvp, const char *name) {
    write_lock(&dvp->rwlock);

    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;
    struct ramfs_entry **prev = &dnode->entries;
    struct ramfs_entry *curr = dnode->entries;

    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            *prev = curr->next;

            struct ramfs_node *fnode = (struct ramfs_node *)curr->vn->data;
            fnode->unlinked = 1;
            fnode->my_entry = NULL;
            curr->vn->v_reclaimable = 1;

            struct vnode *victim = curr->vn;
            kfree(curr);

            write_unlock(&dvp->rwlock);

            vput(victim);
            return 0;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    write_unlock(&dvp->rwlock);
    return -ENOENT;
}

int ramfs_rmdir(struct vnode *dvp, const char *name) {
    write_lock(&dvp->rwlock);

    struct ramfs_node *dnode = (struct ramfs_node *)dvp->data;
    struct ramfs_entry *curr = dnode->entries;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            struct ramfs_node *fnode = (struct ramfs_node *)curr->vn->data;
            if (fnode->entries) {
                write_unlock(&dvp->rwlock);
                return -ENOTEMPTY;
            }
            break;
        }
        curr = curr->next;
    }

    if (!curr) {
        write_unlock(&dvp->rwlock);
        return -ENOENT;
    }

    struct ramfs_entry **prev = &dnode->entries;
    curr = dnode->entries;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            *prev = curr->next;

            struct ramfs_node *fnode = (struct ramfs_node *)curr->vn->data;
            fnode->unlinked = 1;
            fnode->my_entry = NULL;
            curr->vn->v_reclaimable = 1;

            struct vnode *victim = curr->vn;
            kfree(curr);

            write_unlock(&dvp->rwlock);

            vput(victim);
            return 0;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    write_unlock(&dvp->rwlock);
    return -ENOENT;
}

int ramfs_readdir(struct vnode *vp, void *dirent_buf, size_t count, off_t *off) {
    read_lock(&vp->rwlock);

    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    struct ramfs_entry *curr = node->entries;

    for (off_t i = 0; i < *off && curr; i++) {
        curr = curr->next;
    }

    if (!curr) {
        read_unlock(&vp->rwlock);
        return 0;
    }

    struct dirent *de = (struct dirent *)dirent_buf;
    size_t name_len = strlen(curr->name);

    uint16_t reclen = DIRENT_ALIGN(offsetof(struct dirent, d_name) + name_len + 1);

    if (reclen > count) {
        read_unlock(&vp->rwlock);
        return -EINVAL;
    }

    de->d_ino = curr->vn->v_ino;
    de->d_reclen = reclen;
    de->d_type = (curr->vn->type == S_IFDIR) ? DT_DIR : DT_REG;
    de->d_off = (*off) + 1;
    strcpy(de->d_name, curr->name);

    (*off)++;
    read_unlock(&vp->rwlock);
    return (int)reclen;
}

int ramfs_getattr(struct vnode *vp, struct stat *st) {
    read_lock(&vp->rwlock);

    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    memset(st, 0, sizeof(struct stat));

    st->st_size = node->size;
    st->st_mode = vp->type | (node->mode & 07777);
    st->st_uid = node->uid;
    st->st_gid = node->gid;
    st->st_nlink = node->nlink;
    st->st_atim = node->atime;
    st->st_mtim = node->mtime;
    st->st_ctim = node->ctime;
    st->st_rdev = vp->rdev;
    st->st_blksize = 4096;
    st->st_blocks = (node->size + 511) / 512;

    read_unlock(&vp->rwlock);
    return 0;
}

int ramfs_rename(struct vnode *sdvp, const char *sname, struct vnode *tdvp, const char *dname) {
    if (sdvp == tdvp) {
        write_lock(&sdvp->rwlock);
    } else if ((uintptr_t)sdvp < (uintptr_t)tdvp) {
        write_lock(&sdvp->rwlock);
        write_lock(&tdvp->rwlock);
    } else {
        write_lock(&tdvp->rwlock);
        write_lock(&sdvp->rwlock);
    }

    struct ramfs_node *snode = (struct ramfs_node *)sdvp->data;

    struct ramfs_entry **sprev = &snode->entries;
    struct ramfs_entry *scurr = snode->entries;
    struct ramfs_entry *found_entry = NULL;

    while (scurr) {
        if (strcmp(scurr->name, sname) == 0) {
            found_entry = scurr;
            break;
        }
        sprev = &scurr->next;
        scurr = scurr->next;
    }
    if (!found_entry) {
        if (sdvp == tdvp) {
            write_unlock(&sdvp->rwlock);
        } else {
            write_unlock(&sdvp->rwlock);
            write_unlock(&tdvp->rwlock);
        }
        return -ENOENT;
    }

    struct ramfs_node *tnode = (struct ramfs_node *)tdvp->data;
    struct ramfs_entry **tprev = &tnode->entries;
    struct ramfs_entry *tcurr = tnode->entries;
    while (tcurr) {
        if (strcmp(tcurr->name, dname) == 0) {
            *tprev = tcurr->next;
            struct ramfs_node *fnode = (struct ramfs_node *)tcurr->vn->data;
            fnode->unlinked = 1;
            tcurr->vn->v_reclaimable = 1;
            vput(tcurr->vn);
            kfree(tcurr);
            break;
        }
        tprev = &tcurr->next;
        tcurr = tcurr->next;
    }

    *sprev = found_entry->next;

    strncpy(found_entry->name, dname, 255);
    found_entry->name[255] = '\0';
    found_entry->next = tnode->entries;
    tnode->entries = found_entry;

    if (sdvp == tdvp) {
        write_unlock(&sdvp->rwlock);
    } else {
        write_unlock(&sdvp->rwlock);
        write_unlock(&tdvp->rwlock);
    }
    return 0;
}

int ramfs_setattr(struct vnode *vp, struct stat *st) {
    write_lock(&vp->rwlock);

    struct ramfs_node *node = (struct ramfs_node *)vp->data;
    if (vp->type != S_IFREG) {
        write_unlock(&vp->rwlock);
        return -EINVAL;
    }

    if (node->is_static_buf) {
        size_t old_size = node->size;
        size_t num_blocks = (old_size + 4095) / 4096;
        char **new_blocks = NULL;

        if (num_blocks > 0) {
            new_blocks = kmalloc(num_blocks * sizeof(char *));
            if (!new_blocks) { write_unlock(&vp->rwlock); return -ENOMEM; }
            memset(new_blocks, 0, num_blocks * sizeof(char *));

            for (size_t i = 0; i < num_blocks; i++) {
                new_blocks[i] = kmalloc(4096);
                if (!new_blocks[i]) {
                    for (size_t j = 0; j < i; j++) kfree(new_blocks[j]);
                    kfree(new_blocks);
                    write_unlock(&vp->rwlock);
                    return -ENOMEM;
                }

                size_t offset = i * 4096;
                size_t to_copy = 4096;
                if (offset + to_copy > old_size) {
                    to_copy = old_size - offset;
                }

                memcpy(new_blocks[i], node->buffer + offset, to_copy);
                if (to_copy < 4096) {
                    memset(new_blocks[i] + to_copy, 0, 4096 - to_copy);
                }
            }
        }

        node->blocks = new_blocks;
        node->is_static_buf = 0;
    }

    size_t new_size = st->st_size;
    size_t old_size = node->size;

    if (new_size > old_size) {
        size_t old_block_count = (old_size + 4095) / 4096;
        size_t new_block_count = (new_size + 4095) / 4096;

        if (new_block_count > old_block_count) {
            char **new_blocks = krealloc(node->blocks, new_block_count * sizeof(char *));
            if (!new_blocks) { write_unlock(&vp->rwlock); return -ENOMEM; }

            for (size_t i = old_block_count; i < new_block_count; i++) {
                new_blocks[i] = NULL;
            }
            node->blocks = new_blocks;
        }
        node->size = new_size;
    } else if (new_size < old_size) {
        size_t old_block_count = (old_size + 4095) / 4096;
        size_t new_block_count = (new_size + 4095) / 4096;

        if (old_block_count > new_block_count) {
            for (size_t i = new_block_count; i < old_block_count; i++) {
                if (node->blocks[i]) {
                    kfree(node->blocks[i]);
                }
            }
            char **new_blocks = krealloc(node->blocks, new_block_count * sizeof(char *));
            if (new_block_count > 0 && !new_blocks) {
            } else {
                node->blocks = new_blocks;
            }
        }
        if (new_size % 4096 != 0 && node->blocks && node->blocks[new_size / 4096]) {
            memset(node->blocks[new_size / 4096] + (new_size % 4096), 0, 4096 - (new_size % 4096));
        }
        node->size = new_size;
    }

    write_unlock(&vp->rwlock);
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
    .rmdir    = ramfs_rmdir,
    .getattr  = ramfs_getattr,
    .setattr  = ramfs_setattr,
    .readdir  = ramfs_readdir,
    .rename   = ramfs_rename,
};