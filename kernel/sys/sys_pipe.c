#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/syscall.h>
#include <kernel/lock.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>
#include <uapi/fcntl.h>
#include <uapi/sys/stat.h>
#include <string.h>

#define PIPE_BUF_SIZE 4096

struct pipe_buffer {
    char       buf[PIPE_BUF_SIZE];
    size_t     head;
    size_t     tail;
    size_t     size;
    spinlock_t lock;
    int        refcnt;
};

static ssize_t pipe_read(struct vnode *vp, void *buf, size_t n, off_t off) {
    (void)off;
    struct pipe_buffer *pb = (struct pipe_buffer *)vp->data;
    if (!pb) return -EINVAL;

    size_t bytes_read = 0;
    char *dest = (char *)buf;

    while (bytes_read < n) {
        spin_lock(&pb->lock);
        if (pb->size == 0) {
            if (pb->refcnt < 2) {
                spin_unlock(&pb->lock);
                break;
            }
            spin_unlock(&pb->lock);
            thread_yield();
            continue;
        }

        while (pb->size > 0 && bytes_read < n) {
            dest[bytes_read++] = pb->buf[pb->head];
            pb->head = (pb->head + 1) % PIPE_BUF_SIZE;
            pb->size--;
        }
        spin_unlock(&pb->lock);
    }

    return (ssize_t)bytes_read;
}

static ssize_t pipe_write(struct vnode *vp, const void *buf, size_t n, off_t off) {
    (void)off;
    struct pipe_buffer *pb = (struct pipe_buffer *)vp->data;
    if (!pb) return -EINVAL;

    size_t bytes_written = 0;
    const char *src = (const char *)buf;

    while (bytes_written < n) {
        spin_lock(&pb->lock);
        if (pb->refcnt < 2) {
            spin_unlock(&pb->lock);
            return bytes_written > 0 ? (ssize_t)bytes_written : -EPIPE;
        }

        size_t free_space = PIPE_BUF_SIZE - pb->size;
        if (free_space == 0) {
            spin_unlock(&pb->lock);
            thread_yield();
            continue;
        }

        while (free_space > 0 && bytes_written < n) {
            pb->buf[pb->tail] = src[bytes_written++];
            pb->tail = (pb->tail + 1) % PIPE_BUF_SIZE;
            pb->size++;
            free_space--;
        }
        spin_unlock(&pb->lock);
    }

    return (ssize_t)bytes_written;
}

static int pipe_inactive(struct vnode *vp) {
    struct pipe_buffer *pb = (struct pipe_buffer *)vp->data;
    if (pb) {
        spin_lock(&pb->lock);
        pb->refcnt--;
        if (pb->refcnt == 0) {
            spin_unlock(&pb->lock);
            kfree(pb);
        } else {
            spin_unlock(&pb->lock);
        }
    }
    vp->data = NULL;
    return 0;
}

static int pipe_getattr(struct vnode *vp, struct stat *st) {
    memset(st, 0, sizeof(struct stat));
    st->st_mode = S_IFIFO | 0666;
    st->st_nlink = 1;
    struct pipe_buffer *pb = (struct pipe_buffer *)vp->data;
    if (pb) {
        st->st_size = pb->size;
    }
    return 0;
}

static struct vnode_ops pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .inactive = pipe_inactive,
    .getattr = pipe_getattr,
};

int64_t sys_pipe(int *user_pipefd) {
    return sys_pipe2(user_pipefd, 0);
}

int64_t sys_pipe2(int *user_pipefd, int flags) {
    (void)flags;
    if (!is_user_address_range(user_pipefd, sizeof(int) * 2)) {
        return -EFAULT;
    }

    struct pipe_buffer *pb = kmalloc(sizeof(struct pipe_buffer));
    if (!pb) return -ENOMEM;
    memset(pb, 0, sizeof(struct pipe_buffer));
    spin_lock_init(&pb->lock);
    pb->refcnt = 2;

    struct vnode *r_vn = vnode_alloc(S_IFIFO, &pipe_ops);
    if (!r_vn) {
        kfree(pb);
        return -ENOMEM;
    }
    r_vn->data = pb;

    struct vnode *w_vn = vnode_alloc(S_IFIFO, &pipe_ops);
    if (!w_vn) {
        vput(r_vn);
        return -ENOMEM;
    }
    w_vn->data = pb;

    struct file *rf = file_alloc();
    if (!rf) {
        vput(r_vn);
        vput(w_vn);
        return -ENOMEM;
    }
    rf->f_vn = r_vn;
    rf->f_flags = O_RDONLY;

    struct file *wf = file_alloc();
    if (!wf) {
        file_close(rf);
        vput(w_vn);
        return -ENOMEM;
    }
    wf->f_vn = w_vn;
    wf->f_flags = O_WRONLY;

    int r_fd = proc_alloc_fd(curproc, rf);
    if (r_fd < 0) {
        file_close(rf);
        file_close(wf);
        return r_fd;
    }

    int w_fd = proc_alloc_fd(curproc, wf);
    if (w_fd < 0) {
        curproc->p_fd_table[r_fd] = NULL;
        file_close(rf);
        file_close(wf);
        return w_fd;
    }

    int k_fds[2] = { r_fd, w_fd };
    if (copy_to_user(user_pipefd, k_fds, sizeof(int) * 2) < 0) {
        curproc->p_fd_table[r_fd] = NULL;
        curproc->p_fd_table[w_fd] = NULL;
        file_close(rf);
        file_close(wf);
        return -EFAULT;
    }

    return 0;
}
