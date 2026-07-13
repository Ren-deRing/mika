#include <boot/bootinfo.h>

#include <kernel/fs/vfs.h>
#include <kernel/fs/ramfs.h>
#include <kernel/fs/vnode.h>
#include <kernel/fs/file.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>
#include <kernel/printf.h>
#include <kernel/kmem.h>

#include <uapi/fcntl.h>

#include <string.h>

struct cpio_header {
    char c_magic[6];    // 070701
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

static uint32_t hex8_to_u32(const char *s) {
    uint32_t val = 0;
    for (int i = 0; i < 8; i++) {
        val <<= 4;
        if (s[i] >= '0' && s[i] <= '9') val += s[i] - '0';
        else if (s[i] >= 'A' && s[i] <= 'F') val += s[i] - 'A' + 10;
        else if (s[i] >= 'a' && s[i] <= 'f') val += s[i] - 'a' + 10;
    }
    return val;
}

static void mkdir_p(const char *path, mode_t mode) {
    char *temp = kmalloc(4096);
    if (!temp) return;
    strncpy(temp, path, 4095);
    temp[4095] = '\0';
    size_t len = strlen(temp);
    if (len == 0) {
        kfree(temp);
        return;
    }
    if (temp[len - 1] == '/') {
        temp[len - 1] = '\0';
    }
    for (char *p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            vfs_mkdir(temp, mode);
            *p = '/';
        }
    }
    vfs_mkdir(temp, mode);
    kfree(temp);
}

void vfs_load_initrd(uintptr_t addr, uint64_t size) {
    if (!addr || !size) return;

    uint8_t *ptr = (uint8_t *)addr;
    uint8_t *end = ptr + size;

    while (ptr < end) {
        struct cpio_header *h = (struct cpio_header *)ptr;
        if ((uintptr_t)(ptr + sizeof(struct cpio_header)) > (uintptr_t)end) break;
        if (strncmp(h->c_magic, "070701", 6) != 0) break;

        uint32_t filesize = hex8_to_u32(h->c_filesize);
        uint32_t namesize = hex8_to_u32(h->c_namesize);
        uint32_t mode     = hex8_to_u32(h->c_mode);

        char *name = (char *)(ptr + sizeof(struct cpio_header));
        if (strcmp(name, "TRAILER!!!") == 0) break;

        uint32_t head_size = sizeof(struct cpio_header) + namesize;
        uint32_t data_off  = (head_size + 3) & ~3;
        uint8_t *data      = ptr + data_off;

        if ((uintptr_t)(data + ((filesize + 3) & ~3)) > (uintptr_t)end) break;

        char *path = kmalloc(4096);
        if (!path) {
            dprintf("[INITRD] Out of memory allocating path\n");
            break;
        }

        if (name[0] == '.') {
            snprintf(path, 4096, "%s", name + 1);
        } else {
            snprintf(path, 4096, "/%s", name);
        }

        if (S_ISDIR(mode)) {
            mkdir_p(path, mode & 0777); 
        } else if (S_ISLNK(mode)) {
            char *target = kmalloc(filesize + 1);
            if (target) {
                memcpy(target, data, filesize);
                target[filesize] = '\0';
                char *parent = kmalloc(4096);
                if (parent) {
                    strncpy(parent, path, 4095);
                    parent[4095] = '\0';
                    char *last_slash = strrchr(parent, '/');
                    if (last_slash && last_slash != parent) {
                        *last_slash = '\0';
                        mkdir_p(parent, 0755);
                    }
                    kfree(parent);
                }
                int r = vfs_symlink(target, path);
                if (r != 0) {
                    dprintf("[INITRD] Failed to create symlink %s -> %s, error: %d\n", path, target, r);
                }
                kfree(target);
            }
        } else if (S_ISREG(mode)) {
            char *parent = kmalloc(4096);
            if (parent) {
                strncpy(parent, path, 4095);
                parent[4095] = '\0';
                char *last_slash = strrchr(parent, '/');
                if (last_slash && last_slash != parent) {
                    *last_slash = '\0';
                    mkdir_p(parent, 0755);
                }
                kfree(parent);
            }
            int fd;
            int r = vfs_open(path, O_CREAT | O_WRONLY, mode & 0777, &fd);
            if (r != 0) {
                dprintf("[INITRD] Failed to create file %s, error: %d\n", path, r);
            } else {
                struct file *f = fdget(fd);
                if (f) {
                    struct vnode *vp = f->f_vn;
                    if (vp && vp->data) {
                        struct ramfs_node *node = (struct ramfs_node *)vp->data;
                        node->buffer = (char *)data;
                        node->size = filesize;
                        node->is_static_buf = 1;
                    }
                    fdput(f);
                }
                vfs_close(fd);
            }
        }

        kfree(path);
        ptr += data_off + ((filesize + 3) & ~3);
    }
}