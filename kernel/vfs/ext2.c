#include <kernel/fs/vfs.h>
#include <kernel/fs/ext2.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/vnode.h>
#include <kernel/ahci.h>
#include <kernel/blockdev.h>
#include <kernel/blk_cache.h>
#include <kernel/kmem.h>
#include <kernel/printf.h>
#include <kernel/init.h>

#include <uapi/errno.h>
#include <uapi/sys/stat.h>
#include <uapi/sys/dirent.h>

#include <string.h>

#define EXT2_BLOCK_SIZE(fs)  ((fs)->block_size)
#define EXT2_SECTOR_PER_BLOCK(fs) (EXT2_BLOCK_SIZE(fs) / 512)

#define EXT2_VNODE_HASH_SIZE 64

struct ext2_vnode {
    uint32_t ino;
    struct ext2_inode inode;
    struct ext2_fs *fs;
    struct ext2_vnode *hash_next;
    struct vnode *vn;
};

static struct vnode_ops ext2_ops;

static struct vnode *ext2_vnode_cache_lookup(struct ext2_fs *fs, uint32_t ino) {
    uint32_t h = ino % EXT2_VNODE_HASH_SIZE;
    struct ext2_vnode *ev = fs->vnode_hash[h];
    while (ev) {
        if (ev->ino == ino && ev->fs == fs) {
            return ev->vn;
        }
        ev = ev->hash_next;
    }
    return NULL;
}

static void ext2_vnode_cache_insert(struct ext2_fs *fs, struct ext2_vnode *ev) {
    uint32_t h = ev->ino % EXT2_VNODE_HASH_SIZE;
    ev->hash_next = fs->vnode_hash[h];
    fs->vnode_hash[h] = ev;
}

static void ext2_vnode_cache_remove(struct ext2_fs *fs, struct ext2_vnode *ev) {
    uint32_t h = ev->ino % EXT2_VNODE_HASH_SIZE;
    struct ext2_vnode **pp = (struct ext2_vnode **)&fs->vnode_hash[h];
    while (*pp) {
        if (*pp == ev) {
            *pp = ev->hash_next;
            ev->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static int ext2_read_block(struct ext2_fs *fs, uint32_t block, void *buf) {
    uint64_t sector = (uint64_t)block * EXT2_SECTOR_PER_BLOCK(fs);
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < EXT2_SECTOR_PER_BLOCK(fs); i++) {
        if (blk_cache_read(fs->dev_id, sector + i, p + total) < 0)
            return (total > 0) ? total : -1;
        total += 512;
    }
    return total;
}

static int ext2_write_block(struct ext2_fs *fs, uint32_t block, const void *buf) {
    uint64_t sector = (uint64_t)block * EXT2_SECTOR_PER_BLOCK(fs);
    size_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < EXT2_SECTOR_PER_BLOCK(fs); i++) {
        if (blk_cache_write(fs->dev_id, sector + i, p + total) < 0)
            return (total > 0) ? total : -1;
        total += 512;
    }
    return total;
}

static int ext2_read_bg_desc(struct ext2_fs *fs, uint32_t bg, struct ext2_block_group_desc *bgd) {
    uint32_t desc_per_block = fs->desc_per_block;
    uint32_t block = fs->bg_desc_start + (bg / desc_per_block);
    uint32_t offset = (bg % desc_per_block) * sizeof(struct ext2_block_group_desc);
    uint8_t *tmp = kmalloc(fs->block_size);
    if (!tmp) return -ENOMEM;
    int ret = ext2_read_block(fs, block, tmp);
    if (ret < 0) { kfree(tmp); return ret; }
    __builtin_memcpy(bgd, tmp + offset, sizeof(*bgd));
    kfree(tmp);
    return 0;
}

static int ext2_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *inode) {
    if (ino < 1 || ino > fs->inodes_count) return -EINVAL;

    uint32_t bg = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;

    struct ext2_block_group_desc bgd;
    int ret = ext2_read_bg_desc(fs, bg, &bgd);
    if (ret < 0) return ret;

    uint32_t inode_table_block = bgd.bg_inode_table;
    uint32_t inodes_per_block = EXT2_BLOCK_SIZE(fs) / fs->inode_size;
    uint32_t block = inode_table_block + (idx / inodes_per_block);
    uint32_t offset = (idx % inodes_per_block) * fs->inode_size;

    uint8_t *tmp = kmalloc(fs->block_size);
    if (!tmp) return -ENOMEM;
    ret = ext2_read_block(fs, block, tmp);
    if (ret < 0) { kfree(tmp); return ret; }

    __builtin_memcpy(inode, tmp + offset, sizeof(struct ext2_inode));
    kfree(tmp);
    return 0;
}

static int ext2_read_block_ptr(struct ext2_fs *fs,
                               struct ext2_inode *inode,
                               uint32_t block_idx,
                               uint32_t *phys_block) {
    uint32_t ptrs_per_block = EXT2_BLOCK_SIZE(fs) / 4;
    uint32_t ind_ptrs = ptrs_per_block;
    uint32_t dind_ptrs = ptrs_per_block * ptrs_per_block;
    uint32_t tind_ptrs = ptrs_per_block * ptrs_per_block * ptrs_per_block;
    uint32_t *tmp = kmalloc(fs->block_size);
    if (!tmp) return -ENOMEM;

    // Direct blocks
    if (block_idx < 12) {
        *phys_block = inode->i_block[block_idx];
        kfree(tmp);
        return 0;
    }
    block_idx -= 12;

    // Singly indirect
    if (block_idx < ind_ptrs) {
        if (ext2_read_block(fs, inode->i_block[12], tmp) < 0) { kfree(tmp); return -EIO; }
        *phys_block = tmp[block_idx];
        kfree(tmp);
        return 0;
    }
    block_idx -= ind_ptrs;

    // Doubly indirect
    if (block_idx < dind_ptrs) {
        uint32_t ind_idx = block_idx / ptrs_per_block;
        uint32_t blk_idx = block_idx % ptrs_per_block;
        if (ext2_read_block(fs, inode->i_block[13], tmp) < 0) { kfree(tmp); return -EIO; }
        uint32_t ind_block = tmp[ind_idx];
        if (ind_block == 0) { kfree(tmp); *phys_block = 0; return 0; }
        if (ext2_read_block(fs, ind_block, tmp) < 0) { kfree(tmp); return -EIO; }
        *phys_block = tmp[blk_idx];
        kfree(tmp);
        return 0;
    }
    block_idx -= dind_ptrs;

    // Triply indirect
    if (block_idx < tind_ptrs) {
        uint32_t dind_idx = block_idx / dind_ptrs;
        block_idx %= dind_ptrs;
        uint32_t ind_idx = block_idx / ptrs_per_block;
        uint32_t blk_idx = block_idx % ptrs_per_block;
        if (ext2_read_block(fs, inode->i_block[14], tmp) < 0) { kfree(tmp); return -EIO; }
        uint32_t dind_block = tmp[dind_idx];
        if (dind_block == 0) { kfree(tmp); *phys_block = 0; return 0; }
        if (ext2_read_block(fs, dind_block, tmp) < 0) { kfree(tmp); return -EIO; }
        uint32_t ind_block = tmp[ind_idx];
        if (ind_block == 0) { kfree(tmp); *phys_block = 0; return 0; }
        if (ext2_read_block(fs, ind_block, tmp) < 0) { kfree(tmp); return -EIO; }
        *phys_block = tmp[blk_idx];
        kfree(tmp);
        return 0;
    }

    kfree(tmp);
    return -EFBIG;
}

static int ext2_write_bg_desc(struct ext2_fs *fs, uint32_t bg,
                              struct ext2_block_group_desc *bgd) {
    uint32_t block = fs->bg_desc_start + (bg / fs->desc_per_block);
    uint32_t offset = (bg % fs->desc_per_block) * sizeof(struct ext2_block_group_desc);
    uint8_t *tmp = kmalloc(fs->block_size);
    if (!tmp) return -ENOMEM;
    int ret = ext2_read_block(fs, block, tmp);
    if (ret < 0) { kfree(tmp); return ret; }
    __builtin_memcpy(tmp + offset, bgd, sizeof(*bgd));
    ret = ext2_write_block(fs, block, tmp);
    kfree(tmp);
    return ret < 0 ? ret : 0;
}

static int ext2_write_inode(struct ext2_fs *fs, uint32_t ino,
                            struct ext2_inode *inode) {
    if (ino < 1 || ino > fs->inodes_count) return -EINVAL;
    spin_lock(&fs->fs_lock);
    uint32_t bg = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;

    struct ext2_block_group_desc bgd;
    int ret = ext2_read_bg_desc(fs, bg, &bgd);
    if (ret < 0) { spin_unlock(&fs->fs_lock); return ret; }

    uint32_t inode_table_block = bgd.bg_inode_table;
    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t block = inode_table_block + (idx / inodes_per_block);
    uint32_t offset = (idx % inodes_per_block) * fs->inode_size;

    uint8_t *tmp = kmalloc(fs->block_size);
    if (!tmp) { spin_unlock(&fs->fs_lock); return -ENOMEM; }
    ret = ext2_read_block(fs, block, tmp);
    if (ret < 0) { kfree(tmp); spin_unlock(&fs->fs_lock); return ret; }
    __builtin_memcpy(tmp + offset, inode, sizeof(struct ext2_inode));
    ret = ext2_write_block(fs, block, tmp);
    kfree(tmp);
    spin_unlock(&fs->fs_lock);
    return ret < 0 ? ret : 0;
}

static int ext2_alloc_block(struct ext2_fs *fs, uint32_t *new_block) {
    spin_lock(&fs->fs_lock);
    uint32_t num_groups = (fs->blocks_count + fs->blocks_per_group - 1)
                          / fs->blocks_per_group;
    uint32_t bs = fs->block_size;
    uint8_t *bitmap = kmalloc(bs);
    if (!bitmap) { spin_unlock(&fs->fs_lock); return -ENOMEM; }

    for (uint32_t bg = 0; bg < num_groups; bg++) {
        struct ext2_block_group_desc bgd;
        int ret = ext2_read_bg_desc(fs, bg, &bgd);
        if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }
        if (bgd.bg_free_blocks_count == 0) continue;

        ret = ext2_read_block(fs, bgd.bg_block_bitmap, bitmap);
        if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }

        uint32_t start = bg * fs->blocks_per_group;
        uint32_t end = start + fs->blocks_per_group;
        if (end > fs->blocks_count) end = fs->blocks_count;

        for (uint32_t blk = start; blk < end; blk++) {
            if (blk == 0) continue; // no block 0
            uint32_t idx = blk - start;
            uint32_t byte_idx = idx / 8;
            uint32_t bit_idx = idx % 8;
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] |= (1 << bit_idx);
                ret = ext2_write_block(fs, bgd.bg_block_bitmap, bitmap);
                if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }

                bgd.bg_free_blocks_count--;
                ext2_write_bg_desc(fs, bg, &bgd);

                kfree(bitmap);
                *new_block = blk;
                spin_unlock(&fs->fs_lock);
                return 0;
            }
        }
    }
    kfree(bitmap);
    spin_unlock(&fs->fs_lock);
    return -ENOSPC;
}

static int ext2_alloc_inode(struct ext2_fs *fs, uint32_t *new_ino) {
    spin_lock(&fs->fs_lock);
    uint32_t num_groups = (fs->inodes_count + fs->inodes_per_group - 1)
                          / fs->inodes_per_group;
    uint32_t bs = fs->block_size;
    uint8_t *bitmap = kmalloc(bs);
    if (!bitmap) { spin_unlock(&fs->fs_lock); return -ENOMEM; }

    for (uint32_t bg = 0; bg < num_groups; bg++) {
        struct ext2_block_group_desc bgd;
        int ret = ext2_read_bg_desc(fs, bg, &bgd);
        if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }
        if (bgd.bg_free_inodes_count == 0) continue;

        ret = ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap);
        if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }

        uint32_t start_ino = bg * fs->inodes_per_group + 1;
        uint32_t end_ino = start_ino + fs->inodes_per_group;
        if (end_ino > fs->inodes_count + 1)
            end_ino = fs->inodes_count + 1;

        for (uint32_t ino = start_ino; ino < end_ino; ino++) {
            uint32_t idx = ino - 1 - bg * fs->inodes_per_group;
            uint32_t byte_idx = idx / 8;
            uint32_t bit_idx = idx % 8;
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] |= (1 << bit_idx);
                ret = ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap);
                if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }

                bgd.bg_free_inodes_count--;
                ext2_write_bg_desc(fs, bg, &bgd);

                kfree(bitmap);
                *new_ino = ino;
                spin_unlock(&fs->fs_lock);
                return 0;
            }
        }
    }
    kfree(bitmap);
    spin_unlock(&fs->fs_lock);
    return -ENOSPC;
}

static int ext2_set_block_ptr(struct ext2_fs *fs, struct ext2_inode *inode,
                              uint32_t block_idx, uint32_t phys_block) {
    uint32_t ptrs_per_block = fs->block_size / 4;
    uint32_t *tmp = kmalloc(fs->block_size);
    if (!tmp) return -ENOMEM;

    if (block_idx < 12) {
        inode->i_block[block_idx] = phys_block;
        kfree(tmp);
        return 0;
    }
    block_idx -= 12;

    if (block_idx < ptrs_per_block) {
        int fresh = 0;
        if (inode->i_block[12] == 0 && phys_block != 0) {
            uint32_t ind; int r = ext2_alloc_block(fs, &ind);
            if (r < 0) { kfree(tmp); return r; }
            inode->i_block[12] = ind;
            fresh = 1;
        }
        if (inode->i_block[12] == 0) { kfree(tmp); return -EIO; }
        if (fresh) {
            __builtin_memset(tmp, 0, fs->block_size);
        } else {
            int ret = ext2_read_block(fs, inode->i_block[12], tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        tmp[block_idx] = phys_block;
        int ret = ext2_write_block(fs, inode->i_block[12], tmp);
        kfree(tmp);
        return ret < 0 ? ret : 0;
    }
    block_idx -= ptrs_per_block;

    if (block_idx < ptrs_per_block * ptrs_per_block) {
        int dind_fresh = 0;
        if (inode->i_block[13] == 0 && phys_block != 0) {
            uint32_t dind; int r = ext2_alloc_block(fs, &dind);
            if (r < 0) { kfree(tmp); return r; }
            inode->i_block[13] = dind;
            dind_fresh = 1;
        }
        if (inode->i_block[13] == 0) { kfree(tmp); return -EIO; }
        uint32_t ind_idx = block_idx / ptrs_per_block;
        uint32_t blk_idx = block_idx % ptrs_per_block;
        if (dind_fresh) {
            __builtin_memset(tmp, 0, fs->block_size);
        } else {
            int ret = ext2_read_block(fs, inode->i_block[13], tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        int ind_fresh = 0;
        if (tmp[ind_idx] == 0 && phys_block != 0) {
            uint32_t ind; int r = ext2_alloc_block(fs, &ind);
            if (r < 0) { kfree(tmp); return r; }
            tmp[ind_idx] = ind;
            ind_fresh = 1;
            int ret = ext2_write_block(fs, inode->i_block[13], tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        uint32_t ind_block = tmp[ind_idx];
        if (ind_fresh) {
            __builtin_memset(tmp, 0, fs->block_size);
        } else {
            int ret = ext2_read_block(fs, ind_block, tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        tmp[blk_idx] = phys_block;
        int ret = ext2_write_block(fs, ind_block, tmp);
        kfree(tmp);
        return ret < 0 ? ret : 0;
    }
    block_idx -= ptrs_per_block * ptrs_per_block;

    if (block_idx < ptrs_per_block * ptrs_per_block * ptrs_per_block) {
        int tind_fresh = 0;
        if (inode->i_block[14] == 0 && phys_block != 0) {
            uint32_t tind; int r = ext2_alloc_block(fs, &tind);
            if (r < 0) { kfree(tmp); return r; }
            inode->i_block[14] = tind;
            tind_fresh = 1;
        }
        if (inode->i_block[14] == 0) { kfree(tmp); return -EIO; }
        uint32_t dind_ptrs = ptrs_per_block * ptrs_per_block;
        uint32_t dind_idx = block_idx / dind_ptrs;
        block_idx %= dind_ptrs;
        uint32_t ind_idx = block_idx / ptrs_per_block;
        uint32_t blk_idx = block_idx % ptrs_per_block;
        if (tind_fresh) {
            __builtin_memset(tmp, 0, fs->block_size);
        } else {
            int ret = ext2_read_block(fs, inode->i_block[14], tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        int dind_fresh = 0;
        if (tmp[dind_idx] == 0 && phys_block != 0) {
            uint32_t dind; int r = ext2_alloc_block(fs, &dind);
            if (r < 0) { kfree(tmp); return r; }
            tmp[dind_idx] = dind;
            dind_fresh = 1;
            int ret = ext2_write_block(fs, inode->i_block[14], tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        uint32_t dind_block = tmp[dind_idx];
        if (dind_fresh) {
            __builtin_memset(tmp, 0, fs->block_size);
        } else {
            int ret = ext2_read_block(fs, dind_block, tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        int ind_fresh = 0;
        if (tmp[ind_idx] == 0 && phys_block != 0) {
            uint32_t ind; int r = ext2_alloc_block(fs, &ind);
            if (r < 0) { kfree(tmp); return r; }
            tmp[ind_idx] = ind;
            ind_fresh = 1;
            int ret = ext2_write_block(fs, dind_block, tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        uint32_t ind_block = tmp[ind_idx];
        if (ind_fresh) {
            __builtin_memset(tmp, 0, fs->block_size);
        } else {
            int ret = ext2_read_block(fs, ind_block, tmp);
            if (ret < 0) { kfree(tmp); return ret; }
        }
        tmp[blk_idx] = phys_block;
        int ret = ext2_write_block(fs, ind_block, tmp);
        kfree(tmp);
        return ret < 0 ? ret : 0;
    }

    kfree(tmp);
    return -EFBIG;
}

static uint32_t ext2_dirent_size(const char *name) {
    uint32_t namelen = strlen(name);
    uint32_t sz = sizeof(struct ext2_dir_entry) + namelen;
    return (sz + 3) & ~3;
}

static int ext2_add_dirent(struct ext2_fs *fs, struct ext2_vnode *ddev,
                           const char *name, uint32_t ino, uint8_t file_type) {
    uint32_t bs = fs->block_size;
    uint8_t *buf = kmalloc(bs);
    if (!buf) return -ENOMEM;
    int ret;

    uint32_t need = ext2_dirent_size(name);
    uint32_t dir_size = ddev->inode.i_size;

    // Scan existing blocks for space
    for (uint32_t pos = 0; pos <= dir_size; pos += bs) {
        uint32_t block_idx = pos / bs;
        uint32_t phys_block;
        int have_block = 0;

        if (pos < dir_size) {
            ret = ext2_read_block_ptr(fs, &ddev->inode, block_idx, &phys_block);
            if (ret < 0) continue;
            if (phys_block == 0) continue;
            ret = ext2_read_block(fs, phys_block, buf);
            if (ret < 0) continue;
            have_block = 1;
        }

        if (have_block) {
            // Scan entries in this block
            uint32_t off = 0;
            while (off < bs) {
                struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
                uint32_t rec = de->rec_len;
                if (rec == 0) break;

                uint32_t used = (de->inode != 0)
                    ? ((sizeof(struct ext2_dir_entry) + de->name_len + 3) & ~3) : 0;
                uint32_t slack = rec - used;

                if (slack >= need) {
                    // Split this entry
                    if (de->inode != 0) {
                        struct ext2_dir_entry *new_de =
                            (struct ext2_dir_entry *)(buf + off + used);
                        new_de->inode = ino;
                        new_de->rec_len = rec - used;
                        new_de->name_len = strlen(name);
                        new_de->file_type = file_type;
                        __builtin_memcpy(new_de->name, name, new_de->name_len);
                        de->rec_len = used;
                    } else {
                        // Free entry slot — reuse
                        de->inode = ino;
                        de->name_len = strlen(name);
                        de->file_type = file_type;
                        __builtin_memcpy(de->name, name, de->name_len);
                    }
                    ret = ext2_write_block(fs, phys_block, buf);
                    kfree(buf);
                    return (ret < 0) ? ret : 0;
                }
                off += rec;
            }
        }

        // Need a new block
        if (pos == dir_size) {
            uint32_t new_block;
            ret = ext2_alloc_block(fs, &new_block);
            if (ret < 0) { kfree(buf); return ret; }

            // Set the block pointer
            ret = ext2_set_block_ptr(fs, &ddev->inode, block_idx, new_block);
            if (ret < 0) { kfree(buf); return ret; }

            // Update directory size
            ddev->inode.i_size += bs;
            dir_size = ddev->inode.i_size;

            // Fill first entry
            __builtin_memset(buf, 0, bs);
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)buf;
            de->inode = ino;
            de->rec_len = bs;
            de->name_len = strlen(name);
            de->file_type = file_type;
            __builtin_memcpy(de->name, name, de->name_len);

            ret = ext2_write_block(fs, new_block, buf);
            if (ret < 0) { kfree(buf); return ret; }

            // Write back inode with new size
            ext2_write_inode(fs, ddev->ino, &ddev->inode);
            kfree(buf);
            return 0;
        }
    }

    kfree(buf);
    return -ENOSPC;
}

static int ext2_remove_dirent(struct ext2_fs *fs, struct ext2_vnode *ddev,
                              const char *name) {
    uint32_t bs = fs->block_size;
    uint8_t *buf = kmalloc(bs);
    if (!buf) return -ENOMEM;

    uint32_t dir_size = ddev->inode.i_size;

    for (uint32_t pos = 0; pos < dir_size; pos += bs) {
        uint32_t block_idx = pos / bs;
        uint32_t phys_block;
        int ret = ext2_read_block_ptr(fs, &ddev->inode, block_idx, &phys_block);
        if (ret < 0 || phys_block == 0) continue;

        ret = ext2_read_block(fs, phys_block, buf);
        if (ret < 0) continue;

        uint32_t prev_off = 0;
        uint32_t off = 0;
        while (off < bs) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
            if (de->rec_len == 0) break;
            uint32_t next_off = off + de->rec_len;

            if (de->inode != 0 &&
                de->name_len == strlen(name) &&
                __builtin_memcmp(de->name, name, de->name_len) == 0) {
                // Found it — merge into previous entry
                if (prev_off == 0 && off == 0) {
                    // First entry: just mark inode=0
                    de->inode = 0;
                } else {
                    // Merge rec_len into previous
                    struct ext2_dir_entry *prev =
                        (struct ext2_dir_entry *)(buf + prev_off);
                    prev->rec_len += de->rec_len;
                }
                ret = ext2_write_block(fs, phys_block, buf);
                kfree(buf);
                return (ret < 0) ? ret : 0;
            }

            prev_off = off;
            off = next_off;
        }
    }

    kfree(buf);
    return -ENOENT;
}

static void ext2_free_inode(struct ext2_fs *fs, uint32_t ino) {
    if (ino == 0) return;
    spin_lock(&fs->fs_lock);
    uint32_t bg = (ino - 1) / fs->inodes_per_group;
    struct ext2_block_group_desc bgd;
    if (ext2_read_bg_desc(fs, bg, &bgd) < 0) { spin_unlock(&fs->fs_lock); return; }
    uint32_t idx = (ino - 1) % fs->inodes_per_group;
    uint8_t *bmap = kmalloc(fs->block_size);
    if (!bmap) { spin_unlock(&fs->fs_lock); return; }
    if (ext2_read_block(fs, bgd.bg_inode_bitmap, bmap) == 0) {
        uint32_t byte_idx = idx / 8, bit_idx = idx % 8;
        bmap[byte_idx] &= ~(1 << bit_idx);
        ext2_write_block(fs, bgd.bg_inode_bitmap, bmap);
        bgd.bg_free_inodes_count++;
        ext2_write_bg_desc(fs, bg, &bgd);
    }
    kfree(bmap);
    spin_unlock(&fs->fs_lock);
}

static int ext2_free_block_chain(struct ext2_fs *fs, uint32_t block) {
    if (block == 0) return 0;

    spin_lock(&fs->fs_lock);
    uint32_t num_groups = (fs->blocks_count + fs->blocks_per_group - 1)
                          / fs->blocks_per_group;
    uint32_t bs = fs->block_size;
    uint8_t *bitmap = kmalloc(bs);
    if (!bitmap) { spin_unlock(&fs->fs_lock); return -ENOMEM; }

    uint32_t bg = block / fs->blocks_per_group;
    if (bg >= num_groups) { kfree(bitmap); spin_unlock(&fs->fs_lock); return -EINVAL; }

    struct ext2_block_group_desc bgd;
    int ret = ext2_read_bg_desc(fs, bg, &bgd);
    if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }

    ret = ext2_read_block(fs, bgd.bg_block_bitmap, bitmap);
    if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }

    uint32_t idx = block - bg * fs->blocks_per_group;
    uint32_t byte_idx = idx / 8;
    uint32_t bit_idx = idx % 8;
    if (bitmap[byte_idx] & (1 << bit_idx)) {
        bitmap[byte_idx] &= ~(1 << bit_idx);
        ret = ext2_write_block(fs, bgd.bg_block_bitmap, bitmap);
        if (ret < 0) { kfree(bitmap); spin_unlock(&fs->fs_lock); return ret; }
        bgd.bg_free_blocks_count++;
        ext2_write_bg_desc(fs, bg, &bgd);
    }

    kfree(bitmap);
    spin_unlock(&fs->fs_lock);
    return 0;
}

static void ext2_free_indirect_tree(struct ext2_fs *fs, uint32_t block, int depth) {
    if (block == 0) return;
    if (depth == 0) { ext2_free_block_chain(fs, block); return; }

    uint32_t bs = fs->block_size;
    uint32_t ptrs = bs / sizeof(uint32_t);
    uint32_t *buf = kmalloc(bs);
    if (!buf) return;
    if (ext2_read_block(fs, block, buf) < 0) { kfree(buf); return; }

    for (uint32_t i = 0; i < ptrs; i++) {
        if (buf[i]) ext2_free_indirect_tree(fs, buf[i], depth - 1);
    }
    kfree(buf);
    ext2_free_block_chain(fs, block);
}

// Free all blocks of an inode
static int ext2_truncate_blocks(struct ext2_fs *fs, struct ext2_inode *inode,
                                uint32_t new_size) {
    uint32_t bs = fs->block_size;
    uint32_t ptrs = bs / sizeof(uint32_t);
    uint32_t old_blocks = (inode->i_size + bs - 1) / bs;
    uint32_t new_blocks = (new_size + bs - 1) / bs;

    for (uint32_t i = new_blocks; i < old_blocks && i < 12; i++) {
        if (inode->i_block[i]) {
            ext2_free_block_chain(fs, inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }

    if (old_blocks > 12 && new_blocks <= 12) {
        ext2_free_indirect_tree(fs, inode->i_block[12], 1);
        inode->i_block[12] = 0;
    } else if (old_blocks > 12 + ptrs && new_blocks > 12 && new_blocks <= 12 + ptrs) {
        uint32_t start = new_blocks - 12;
        uint32_t *buf = kmalloc(bs);
        if (buf && inode->i_block[12] && ext2_read_block(fs, inode->i_block[12], buf) == 0) {
            for (uint32_t i = start / ptrs; i < ptrs; i++) {
                if (buf[i]) ext2_free_block_chain(fs, buf[i]);
            }
            kfree(buf);
        } else { kfree(buf); }
        if (new_blocks == 12) {
            ext2_free_block_chain(fs, inode->i_block[12]);
            inode->i_block[12] = 0;
        }
    }

    if (old_blocks > 12 + ptrs && new_blocks <= 12 + ptrs) {
        ext2_free_indirect_tree(fs, inode->i_block[13], 2);
        inode->i_block[13] = 0;
    }

    if (old_blocks > 12 + ptrs + ptrs * ptrs && new_blocks <= 12 + ptrs + ptrs * ptrs) {
        ext2_free_indirect_tree(fs, inode->i_block[14], 3);
        inode->i_block[14] = 0;
    }

    inode->i_size = new_size;
    return 0;
}

static struct vnode *ext2_create_vnode(struct ext2_fs *fs,
                                        uint32_t ino,
                                        struct ext2_inode *inode) {
    uint32_t type;
    if (inode->i_mode & EXT2_S_IFDIR)      type = S_IFDIR;
    else if (inode->i_mode & EXT2_S_IFREG) type = S_IFREG;
    else if (inode->i_mode & EXT2_S_IFLNK) type = S_IFLNK;
    else if (inode->i_mode & EXT2_S_IFBLK) type = S_IFBLK;
    else if (inode->i_mode & EXT2_S_IFCHR) type = S_IFCHR;
    else                                    type = S_IFREG;

    struct vnode *vn = vnode_alloc(type, &ext2_ops);
    if (!vn) return NULL;

    struct ext2_vnode *ev = kmalloc(sizeof(struct ext2_vnode));
    if (!ev) { vput(vn); return NULL; }

    ev->ino = ino;
    ev->fs = fs;
    ev->vn = vn;
    __builtin_memcpy(&ev->inode, inode, sizeof(struct ext2_inode));
    vn->data = ev;

    return vn;
}

static int ext2_lookup(struct vnode *dvp, const char *name, struct vnode **vpp) {
    if (dvp->type != S_IFDIR) return -ENOTDIR;

    struct ext2_vnode *dev = (struct ext2_vnode *)dvp->data;
    if (!dev) return -ENXIO;
    struct ext2_fs *fs = dev->fs;

    uint32_t size = dev->inode.i_size;
    uint32_t pos = 0;
    uint32_t block_size = EXT2_BLOCK_SIZE(fs);
    uint8_t *buf = kmalloc(block_size);
    if (!buf) return -ENOMEM;

    while (pos < size) {
        uint32_t block_idx = pos / block_size;
        uint32_t block_off = pos % block_size;
        uint32_t phys_block;
        if (ext2_read_block_ptr(fs, &dev->inode, block_idx, &phys_block) < 0)
            { kfree(buf); return -EIO; }
        if (phys_block == 0) {
            pos += block_size - block_off;
            continue;
        }
        if (ext2_read_block(fs, phys_block, buf) < 0)
            { kfree(buf); return -EIO; }

        uint32_t off = block_off;
        while (off < block_size && pos < size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
            if (de->rec_len == 0) break;
            if (de->inode == 0) {
                off += de->rec_len;
                pos += de->rec_len;
                continue;
            }
            uint32_t name_len = de->name_len;
            if (name_len == strlen(name) &&
                __builtin_memcmp(de->name, name, name_len) == 0) {
                uint32_t found_ino = de->inode;
                kfree(buf);

                spin_lock(&fs->vnode_hash_lock);
                struct vnode *cached = ext2_vnode_cache_lookup(fs, found_ino);
                if (cached) {
                    spin_unlock(&fs->vnode_hash_lock);
                    vref(cached);
                    *vpp = cached;
                    return 0;
                }
                spin_unlock(&fs->vnode_hash_lock);

                struct ext2_inode inode;
                if (ext2_read_inode(fs, found_ino, &inode) < 0)
                    return -EIO;
                struct vnode *vn = ext2_create_vnode(fs, found_ino, &inode);
                if (!vn) return -ENOMEM;
                struct ext2_vnode *nev = (struct ext2_vnode *)vn->data;
                spin_lock(&fs->vnode_hash_lock);
                ext2_vnode_cache_insert(fs, nev);
                spin_unlock(&fs->vnode_hash_lock);
                *vpp = vn;
                return 0;
            }
            off += de->rec_len;
            pos += de->rec_len;
        }
    }

    kfree(buf);
    return -ENOENT;
}

static int ext2_readdir(struct vnode *dvp, void *dirent_buf, size_t count, off_t *off) {
    if (dvp->type != S_IFDIR) return -ENOTDIR;

    struct ext2_vnode *dev = (struct ext2_vnode *)dvp->data;
    if (!dev) return -ENXIO;
    struct ext2_fs *fs = dev->fs;

    uint32_t size = dev->inode.i_size;
    uint32_t pos = 0;
    uint32_t block_size = EXT2_BLOCK_SIZE(fs);
    uint8_t *buf = kmalloc(block_size);
    if (!buf) return -ENOMEM;
    int entry_idx = 0;

    while (pos < size) {
        uint32_t block_idx = pos / block_size;
        uint32_t block_off = pos % block_size;
        uint32_t phys_block;
        if (ext2_read_block_ptr(fs, &dev->inode, block_idx, &phys_block) < 0)
            { kfree(buf); return -EIO; }
        if (phys_block == 0) {
            pos += block_size - block_off;
            continue;
        }
        if (ext2_read_block(fs, phys_block, buf) < 0)
            { kfree(buf); return -EIO; }

        while (block_off < block_size && pos < size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + block_off);
            if (de->rec_len == 0) { pos = size; break; }
            if (de->inode != 0) {
                if (entry_idx == *off) {
                    struct dirent *d = (struct dirent *)dirent_buf;
                    size_t name_len = de->name_len;
                    if (name_len > 255) name_len = 255;
                    size_t reclen = sizeof(struct dirent) + name_len + 1;
                    if (reclen > count) { kfree(buf); return 0; }
                    d->d_ino = de->inode;
                    d->d_reclen = reclen;
                    d->d_type = de->file_type;
                    __builtin_memcpy(d->d_name, de->name, name_len);
                    d->d_name[name_len] = '\0';
                    *off = (off_t)(entry_idx + 1);
                    kfree(buf);
                    return reclen;
                }
                entry_idx++;
            }
            block_off += de->rec_len;
            pos += de->rec_len;
        }
    }

    kfree(buf);
    return 0;
}

static ssize_t ext2_read(struct vnode *vp, void *buf, size_t count, off_t off) {
    struct ext2_vnode *dev = (struct ext2_vnode *)vp->data;
    if (!dev) return -ENXIO;

    mutex_lock(&vp->io_mutex);

    struct ext2_fs *fs = dev->fs;

    uint32_t file_size = dev->inode.i_size;
    if ((uint64_t)off >= file_size) { mutex_unlock(&vp->io_mutex); return 0; }
    if ((uint64_t)off + count > file_size)
        count = file_size - (uint64_t)off;

    uint32_t block_size = EXT2_BLOCK_SIZE(fs);
    size_t done = 0;
    uint8_t *tmp = kmalloc(block_size > 4096 ? block_size : 4096);
    if (!tmp) { mutex_unlock(&vp->io_mutex); return -ENOMEM; }

    while (done < count) {
        uint64_t byte_pos = (uint64_t)off + done;
        uint32_t block_idx = (uint32_t)(byte_pos / block_size);
        uint32_t block_off = (uint32_t)(byte_pos % block_size);

        uint32_t phys_block;
        if (ext2_read_block_ptr(fs, &dev->inode, block_idx, &phys_block) < 0)
            { kfree(tmp); mutex_unlock(&vp->io_mutex); return done ? done : -EIO; }
        if (phys_block == 0) {
            // sparse file — fill with zeros
            size_t chunk = block_size - block_off;
            if (chunk > count - done) chunk = count - done;
            __builtin_memset((uint8_t *)buf + done, 0, chunk);
            done += chunk;
            continue;
        }

        if (ext2_read_block(fs, phys_block, tmp) < 0)
            { kfree(tmp); mutex_unlock(&vp->io_mutex); return done ? done : -EIO; }

        size_t chunk = block_size - block_off;
        if (chunk > count - done) chunk = count - done;
        __builtin_memcpy((uint8_t *)buf + done, tmp + block_off, chunk);
        done += chunk;
    }

    kfree(tmp);
    mutex_unlock(&vp->io_mutex);
    return done;
}

static int ext2_getattr(struct vnode *vp, struct stat *st) {
    struct ext2_vnode *dev = (struct ext2_vnode *)vp->data;
    if (!dev) return -ENXIO;

    read_lock(&vp->rwlock);

    __builtin_memset(st, 0, sizeof(*st));

    st->st_mode = dev->inode.i_mode & 0xFFFF;
    st->st_uid  = dev->inode.i_uid;
    st->st_gid  = dev->inode.i_gid;
    st->st_size = dev->inode.i_size;
    st->st_blocks = dev->inode.i_blocks;
    st->st_blksize = EXT2_BLOCK_SIZE(dev->fs);
    st->st_atime = dev->inode.i_atime;
    st->st_mtime = dev->inode.i_mtime;
    st->st_ctime = dev->inode.i_ctime;
    st->st_nlink = dev->inode.i_links_count;
    st->st_ino   = dev->ino;

    read_unlock(&vp->rwlock);
    return 0;
}

static int ext2_inactive(struct vnode *vp) {
    struct ext2_vnode *dev = (struct ext2_vnode *)vp->data;
    if (dev) {
        // Remove from vnode cache
        spin_lock(&dev->fs->vnode_hash_lock);
        ext2_vnode_cache_remove(dev->fs, dev);
        spin_unlock(&dev->fs->vnode_hash_lock);
        // Write back inode before freeing
        if (vp->v_reclaimable && dev->ino)
            ext2_write_inode(dev->fs, dev->ino, &dev->inode);
        kfree(dev);
        vp->data = NULL;
    }
    return 0;
}

static ssize_t ext2_write(struct vnode *vp, const void *buf,
                          size_t count, off_t off) {
    struct ext2_vnode *dev = (struct ext2_vnode *)vp->data;
    if (!dev) return -ENXIO;
    if (vp->type != S_IFREG) return -EISDIR;

    write_lock(&vp->rwlock);
    mutex_lock(&vp->io_mutex);

    struct ext2_fs *fs = dev->fs;

    uint32_t bs = fs->block_size;
    uint32_t new_size = (uint32_t)((uint64_t)off + count);
    if (new_size > dev->inode.i_size) {
        // Extend file — allocate any new blocks needed
        uint32_t old_blocks = (dev->inode.i_size + bs - 1) / bs;
        uint32_t new_blocks = (new_size + bs - 1) / bs;
        for (uint32_t bi = old_blocks; bi < new_blocks; bi++) {
            uint32_t blk;
            int ret = ext2_alloc_block(fs, &blk);
            if (ret < 0) { mutex_unlock(&vp->io_mutex); write_unlock(&vp->rwlock); return ret; }
            ret = ext2_set_block_ptr(fs, &dev->inode, bi, blk);
            if (ret < 0) { mutex_unlock(&vp->io_mutex); write_unlock(&vp->rwlock); return ret; }
        }
        dev->inode.i_size = new_size;
    }

    uint8_t *tmp = kmalloc(bs > 4096 ? bs : 4096);
    if (!tmp) { mutex_unlock(&vp->io_mutex); write_unlock(&vp->rwlock); return -ENOMEM; }
    size_t done = 0;

    while (done < count) {
        uint64_t byte_pos = (uint64_t)off + done;
        uint32_t block_idx = (uint32_t)(byte_pos / bs);
        uint32_t block_off = (uint32_t)(byte_pos % bs);

        uint32_t phys_block;
        int ret = ext2_read_block_ptr(fs, &dev->inode, block_idx, &phys_block);
        if (ret < 0) { kfree(tmp); mutex_unlock(&vp->io_mutex); write_unlock(&vp->rwlock); return done ? done : ret; }

        size_t chunk = bs - block_off;
        if (chunk > count - done) chunk = count - done;

        if (chunk < bs) {
            // Partial block: read-modify-write
            ext2_read_block(fs, phys_block, tmp);
        }
        // For full block writes, skip the read
        __builtin_memcpy(tmp + block_off, (const uint8_t *)buf + done, chunk);
        ext2_write_block(fs, phys_block, tmp);
        done += chunk;
    }

    kfree(tmp);

    // Update timestamps and write inode
    dev->inode.i_mtime = 0; // TODO: real timestamp
    dev->inode.i_ctime = 0;
    uint32_t alloc_blocks = (dev->inode.i_size + bs - 1) / bs;
    dev->inode.i_blocks = alloc_blocks * (bs / 512);
    ext2_write_inode(fs, dev->ino, &dev->inode);

    mutex_unlock(&vp->io_mutex);
    write_unlock(&vp->rwlock);
    return count;
}

static int ext2_create(struct vnode *dvp, const char *name,
                       mode_t mode, struct vnode **vpp) {
    if (dvp->type != S_IFDIR) return -ENOTDIR;

    write_lock(&dvp->rwlock);

    struct ext2_vnode *ddev = (struct ext2_vnode *)dvp->data;
    if (!ddev) { write_unlock(&dvp->rwlock); return -ENXIO; }
    struct ext2_fs *fs = ddev->fs;

    // Check for existing entry
    struct vnode *existing;
    if (ext2_lookup(dvp, name, &existing) == 0) {
        vput(existing);
        write_unlock(&dvp->rwlock);
        return -EEXIST;
    }

    // Allocate inode
    uint32_t ino;
    int ret = ext2_alloc_inode(fs, &ino);
    if (ret < 0) { write_unlock(&dvp->rwlock); return ret; }

    // Initialize inode
    struct ext2_inode inode;
    __builtin_memset(&inode, 0, sizeof(inode));
    uint32_t type_mask = mode & S_IFMT;
    if (type_mask == S_IFDIR)
        inode.i_mode = EXT2_S_IFDIR | (mode & 07777);
    else if (type_mask == S_IFLNK)
        inode.i_mode = EXT2_S_IFLNK | (mode & 07777);
    else if (type_mask == S_IFCHR)
        inode.i_mode = EXT2_S_IFCHR | (mode & 07777);
    else if (type_mask == S_IFBLK)
        inode.i_mode = EXT2_S_IFBLK | (mode & 07777);
    else
        inode.i_mode = EXT2_S_IFREG | (mode & 07777);
    inode.i_uid = curproc ? curproc->p_euid : 0;
    inode.i_gid = curproc ? curproc->p_egid : 0;
    inode.i_links_count = 1;
    inode.i_size = 0;
    inode.i_atime = 0;
    inode.i_ctime = 0;
    inode.i_mtime = 0;
    inode.i_dtime = 0;
    inode.i_blocks = 0;
    __builtin_memset(inode.i_block, 0, sizeof(inode.i_block));

    // Write inode
    ret = ext2_write_inode(fs, ino, &inode);
    if (ret < 0) {
        ext2_free_inode(fs, ino);
        write_unlock(&dvp->rwlock);
        return ret;
    }

    // Add directory entry
    uint8_t file_type;
    if (inode.i_mode & EXT2_S_IFDIR) file_type = EXT2_FT_DIR;
    else if (inode.i_mode & EXT2_S_IFREG) file_type = EXT2_FT_REG_FILE;
    else if (inode.i_mode & EXT2_S_IFLNK) file_type = EXT2_FT_SYMLINK;
    else if (inode.i_mode & EXT2_S_IFCHR) file_type = EXT2_FT_CHRDEV;
    else if (inode.i_mode & EXT2_S_IFBLK) file_type = EXT2_FT_BLKDEV;
    else file_type = EXT2_FT_UNKNOWN;

    ret = ext2_add_dirent(fs, ddev, name, ino, file_type);
    if (ret < 0) {
        ext2_free_inode(fs, ino);
        write_unlock(&dvp->rwlock);
        return ret;
    }

    // Create vnode
    struct vnode *vn = ext2_create_vnode(fs, ino, &inode);
    if (!vn) {
        ext2_free_inode(fs, ino);
        write_unlock(&dvp->rwlock);
        return -ENOMEM;
    }

    // Insert into vnode cache
    struct ext2_vnode *nev = (struct ext2_vnode *)vn->data;
    spin_lock(&fs->vnode_hash_lock);
    ext2_vnode_cache_insert(fs, nev);
    spin_unlock(&fs->vnode_hash_lock);

    // For directories, create . and ..
    if (inode.i_mode & EXT2_S_IFDIR) {
        struct ext2_vnode *ndev = (struct ext2_vnode *)vn->data;
        ext2_add_dirent(fs, ndev, ".", ino, EXT2_FT_DIR);
        ext2_add_dirent(fs, ndev, "..", ddev->ino, EXT2_FT_DIR);
        ndev->inode.i_links_count = 2;
        ddev->inode.i_links_count++;
        ext2_write_inode(fs, ddev->ino, &ddev->inode);
        ext2_write_inode(fs, ino, &ndev->inode);
    }

    vref(vn);
    *vpp = vn;
    write_unlock(&dvp->rwlock);
    return 0;
}

static int ext2_mkdir(struct vnode *dvp, const char *name, mode_t mode) {
    struct vnode *vp;
    int ret = ext2_create(dvp, name, S_IFDIR | (mode & 07777), &vp);
    if (ret == 0) vput(vp);
    return ret;
}

static int ext2_remove(struct vnode *dvp, const char *name) {
    if (dvp->type != S_IFDIR) return -ENOTDIR;

    write_lock(&dvp->rwlock);

    struct ext2_vnode *ddev = (struct ext2_vnode *)dvp->data;
    if (!ddev) { write_unlock(&dvp->rwlock); return -ENXIO; }
    struct ext2_fs *fs = ddev->fs;

    // Find the inode number
    struct vnode *vp;
    int ret = ext2_lookup(dvp, name, &vp);
    if (ret < 0) { write_unlock(&dvp->rwlock); return ret; }
    struct ext2_vnode *ev = (struct ext2_vnode *)vp->data;
    uint32_t ino = ev->ino;

    // Remove directory entry
    ret = ext2_remove_dirent(fs, ddev, name);
    if (ret < 0) { vput(vp); write_unlock(&dvp->rwlock); return ret; }

    // Decrement nlink
    ev->inode.i_links_count--;
    if (ev->inode.i_links_count == 0) {
        // Free blocks and inode
        ext2_truncate_blocks(fs, &ev->inode, 0);
        // Free inode via bitmap
        ext2_free_inode(fs, ino);
        ev->inode.i_dtime = 0; // deletion time
        ev->inode.i_links_count = 0;
    }
    ext2_write_inode(fs, ino, &ev->inode);

    // Remove from vnode cache
    spin_lock(&fs->vnode_hash_lock);
    ext2_vnode_cache_remove(fs, ev);
    spin_unlock(&fs->vnode_hash_lock);

    vp->v_reclaimable = 1;
    vput(vp);
    write_unlock(&dvp->rwlock);
    return 0;
}

static int ext2_rmdir(struct vnode *dvp, const char *name) {
    if (dvp->type != S_IFDIR) return -ENOTDIR;

    write_lock(&dvp->rwlock);

    // Check directory is empty
    struct vnode *vp;
    int ret = ext2_lookup(dvp, name, &vp);
    if (ret < 0) { write_unlock(&dvp->rwlock); return ret; }
    struct ext2_vnode *ev = (struct ext2_vnode *)vp->data;

    uint32_t dir_size = ev->inode.i_size;
    uint32_t bs = ev->fs->block_size;
    if (dir_size > bs * 2) {
        vput(vp);
        write_unlock(&dvp->rwlock);
        return -ENOTEMPTY;
    }

    // Count entries by readdir
    off_t off = 0;
    char dbuf[256];
    while (1) {
        int r = ext2_readdir(vp, dbuf, sizeof(dbuf), &off);
        if (r <= 0) break;
        struct dirent *d = (struct dirent *)dbuf;
        if (d->d_name[0] != '.' ||
            (d->d_name[1] != '\0' && !(d->d_name[1] == '.' && d->d_name[2] == '\0'))) {
            vput(vp);
            write_unlock(&dvp->rwlock);
            return -ENOTEMPTY;
        }
    }

    vput(vp);

    // Remove the directory entry
    struct ext2_vnode *ddev = (struct ext2_vnode *)dvp->data;
    ret = ext2_remove_dirent(ddev->fs, ddev, name);
    if (ret < 0) { write_unlock(&dvp->rwlock); return ret; }

    // Decrement parent link count
    ddev->inode.i_links_count--;
    ext2_write_inode(ddev->fs, ddev->ino, &ddev->inode);

    write_unlock(&dvp->rwlock);
    return 0;
}

static int ext2_setattr(struct vnode *vp, struct stat *st) {
    struct ext2_vnode *dev = (struct ext2_vnode *)vp->data;
    if (!dev) return -ENXIO;

    write_lock(&vp->rwlock);

    if (st->st_mode != (mode_t)-1) {
        dev->inode.i_mode = (dev->inode.i_mode & ~07777u) | (st->st_mode & 07777u);
    }
    if (st->st_uid != (uid_t)-1) {
        dev->inode.i_uid = st->st_uid;
    }
    if (st->st_gid != (gid_t)-1) {
        dev->inode.i_gid = st->st_gid;
    }

    if (st->st_size >= 0 && (uint64_t)st->st_size != dev->inode.i_size) {
        ext2_truncate_blocks(dev->fs, &dev->inode, (uint32_t)st->st_size);
    }

    ext2_write_inode(dev->fs, dev->ino, &dev->inode);

    write_unlock(&vp->rwlock);
    return 0;
}

static struct vnode *ext2_mount(struct vnode *dev_vp, void *data) {
    (void)data;
    if (!dev_vp || !S_ISBLK(dev_vp->type)) {
        dprintf("[EXT2] Mount requires a block device\n");
        return NULL;
    }

    int dev_id = blockdev_get_devid(dev_vp);
    if (dev_id < 0) {
        dprintf("[EXT2] Failed to get block device id\n");
        return NULL;
    }

    struct ext2_fs *fs = kmalloc(sizeof(struct ext2_fs));
    if (!fs) return NULL;
    __builtin_memset(fs, 0, sizeof(*fs));
    fs->dev_id = dev_id;
    spin_lock_init(&fs->fs_lock);
    spin_lock_init(&fs->vnode_hash_lock);
    __builtin_memset(fs->vnode_hash, 0, sizeof(fs->vnode_hash));

    // Read superblock (offset 1024 = sector 2)
    uint8_t sb_buf[1024];
    if (ahci_bread(fs->dev_id, 2, sb_buf) < 0) {
        dprintf("[EXT2] Failed to read superblock sector\n");
        kfree(fs);
        return NULL;
    }

    struct ext2_superblock *sb = (struct ext2_superblock *)sb_buf;
    if (sb->s_magic != EXT2_SUPER_MAGIC) {
        dprintf("[EXT2] Bad superblock magic: 0x%04x (expected 0xEF53)\n", sb->s_magic);
        kfree(fs);
        return NULL;
    }

    fs->block_size = 1024 << sb->s_log_block_size;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->inode_size = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->inodes_count = sb->s_inodes_count;
    fs->blocks_count = sb->s_blocks_count;
    fs->desc_per_block = fs->block_size / sizeof(struct ext2_block_group_desc);

    // Block group descriptors start at block 1 (for 1KB blocks) or block 0+1
    if (fs->block_size == 1024) {
        fs->bg_desc_start = 2; // block 0=super? no: super in block 1, bg_desc in block 2
    } else {
        fs->bg_desc_start = 1;
    }

    // Read root inode (inode 2)
    struct ext2_inode root_inode;
    if (ext2_read_inode(fs, EXT2_ROOT_INO, &root_inode) < 0) {
        dprintf("[EXT2] Failed to read root inode\n");
        kfree(fs);
        return NULL;
    }

    struct vnode *root_vn = ext2_create_vnode(fs, EXT2_ROOT_INO, &root_inode);
    if (!root_vn) {
        kfree(fs);
        return NULL;
    }

    return root_vn;
}

static struct vnode_ops ext2_ops = {
    .lookup   = ext2_lookup,
    .readdir  = ext2_readdir,
    .read     = ext2_read,
    .write    = ext2_write,
    .create   = ext2_create,
    .mkdir    = ext2_mkdir,
    .remove   = ext2_remove,
    .rmdir    = ext2_rmdir,
    .getattr  = ext2_getattr,
    .setattr  = ext2_setattr,
    .inactive = ext2_inactive,
};

static struct file_system_type ext2_fs_type = {
    .name  = "ext2",
    .mount = ext2_mount,
};

static void ext2_fs_init(void) {
    register_filesystem(&ext2_fs_type);
}

subsys_initcall(ext2_fs_init, PRIO_THIRD);

static void ext2_root_mount(void) {
    int err = vfs_mount("/dev/sda", "/mnt", "ext2", NULL);
    if (err != 0) {
        dprintf("[EXT2] mount failed: %d (disk may not be formatted)\n", err);
    }
}

late_initcall(ext2_root_mount, PRIO_FOURTH);
