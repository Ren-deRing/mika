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
#include <kernel/wait.h>
#include <uapi/fcntl.h>
#include <uapi/sys/stat.h>
#include <string.h>

#define PIPE_BUF_SIZE 4096

struct pipe_buffer {
    char            buf[PIPE_BUF_SIZE];
    size_t          head;
    size_t          tail;
    size_t          size;
    spinlock_t      lock;
    int             refcnt;
    wait_queue_head_t rwaitq; /* reader waiters */
    wait_queue_head_t wwaitq; /* writer waiters */
};

static ssize_t pipe_read(struct vnode *vp, void *buf, size_t n, off_t off) {
    (void)off;
    struct pipe_buffer *pb = (struct pipe_buffer *)vp->data;
    if (!pb) return -EINVAL;

    size_t bytes_read = 0;

    while (bytes_read < n) {
        spin_lock(&pb->lock);

        while (pb->size == 0 && pb->refcnt >= 2) {
            curthread->t_state = THREAD_WAITING;
            add_wait_queue(&pb->wwaitq, curthread);
            spin_unlock(&pb->lock);
            thread_yield();
            spin_lock(&pb->lock);
        }
        remove_wait_queue(&pb->wwaitq, curthread);

        if (pb->size == 0) {
            spin_unlock(&pb->lock);
            break;
        }

        size_t to_read = n - bytes_read;
        if (to_read > pb->size) to_read = pb->size;

        char kbuf[256];
        size_t chunk = to_read > sizeof(kbuf) ? sizeof(kbuf) : to_read;
        for (size_t i = 0; i < chunk; i++) {
            kbuf[i] = pb->buf[pb->head];
            pb->head = (pb->head + 1) % PIPE_BUF_SIZE;
            pb->size--;
        }
        spin_unlock(&pb->lock);

        if (copy_to_user((char *)buf + bytes_read, kbuf, chunk) < 0)
            return bytes_read > 0 ? (ssize_t)bytes_read : -EFAULT;
        bytes_read += chunk;

        wake_up(&pb->rwaitq);
    }

    return (ssize_t)bytes_read;
}

static ssize_t pipe_write(struct vnode *vp, const void *buf, size_t n, off_t off) {
    (void)off;
    struct pipe_buffer *pb = (struct pipe_buffer *)vp->data;
    if (!pb) return -EINVAL;

    size_t bytes_written = 0;

    while (bytes_written < n) {
        size_t to_write = n - bytes_written;
        char kbuf[256];
        size_t chunk = to_write > sizeof(kbuf) ? sizeof(kbuf) : to_write;
        if (copy_from_user(kbuf, (const char *)buf + bytes_written, chunk) < 0)
            return bytes_written > 0 ? (ssize_t)bytes_written : -EFAULT;

        spin_lock(&pb->lock);

        while (pb->refcnt >= 2 && PIPE_BUF_SIZE - pb->size < chunk) {
            curthread->t_state = THREAD_WAITING;
            add_wait_queue(&pb->rwaitq, curthread);
            spin_unlock(&pb->lock);
            thread_yield();
            spin_lock(&pb->lock);
        }
        remove_wait_queue(&pb->rwaitq, curthread);

        if (pb->refcnt < 2) {
            spin_unlock(&pb->lock);
            return bytes_written > 0 ? (ssize_t)bytes_written : -EPIPE;
        }

        size_t free_space = PIPE_BUF_SIZE - pb->size;
        if (free_space < chunk) chunk = free_space;

        for (size_t i = 0; i < chunk; i++) {
            pb->buf[pb->tail] = kbuf[i];
            pb->tail = (pb->tail + 1) % PIPE_BUF_SIZE;
            pb->size++;
        }
        bytes_written += chunk;

        spin_unlock(&pb->lock);
        wake_up(&pb->wwaitq);
    }

    return (ssize_t)bytes_written;
}

static int pipe_inactive(struct vnode *vp) {
    struct pipe_buffer *pb = (struct pipe_buffer *)vp->data;
    if (pb) {
        spin_lock(&pb->lock);
        pb->refcnt--;
        int last = (pb->refcnt == 0);
        spin_unlock(&pb->lock);

        wake_up_all(&pb->rwaitq);
        wake_up_all(&pb->wwaitq);

        if (last) {
            kfree(pb);
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

struct vnode_ops pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .inactive = pipe_inactive,
    .getattr = pipe_getattr,
};

int64_t sys_pipe(int *user_pipefd) {
    return sys_pipe2(user_pipefd, 0);
}

int64_t sys_pipe2(int *user_pipefd, int flags) {
    if (!is_user_address_range(user_pipefd, sizeof(int) * 2)) {
        return -EFAULT;
    }

    struct pipe_buffer *pb = kmalloc(sizeof(struct pipe_buffer));
    if (!pb) return -ENOMEM;
    memset(pb, 0, sizeof(struct pipe_buffer));
    spin_lock_init(&pb->lock);
    init_waitqueue_head(&pb->rwaitq);
    init_waitqueue_head(&pb->wwaitq);
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
    if (flags & O_NONBLOCK) {
        rf->f_flags |= O_NONBLOCK;
    }
    if (flags & 0x80000) { // O_CLOEXEC
        rf->f_flags |= 0x80000;
    }

    struct file *wf = file_alloc();
    if (!wf) {
        file_close(rf);
        vput(w_vn);
        return -ENOMEM;
    }
    wf->f_vn = w_vn;
    wf->f_flags = O_WRONLY;
    if (flags & O_NONBLOCK) {
        wf->f_flags |= O_NONBLOCK;
    }
    if (flags & 0x80000) { // O_CLOEXEC
        wf->f_flags |= 0x80000;
    }

    int r_fd = proc_alloc_fd(curproc, rf);
    if (r_fd < 0) {
        file_close(rf);
        file_close(wf);
        return r_fd;
    }
    file_close(rf);

    int w_fd = proc_alloc_fd(curproc, wf);
    if (w_fd < 0) {
        spin_lock(&curproc->p_lock);
        curproc->p_fd_table[r_fd] = NULL;
        spin_unlock(&curproc->p_lock);
        file_close(rf);
        file_close(wf);
        return w_fd;
    }
    file_close(wf);

    int k_fds[2] = { r_fd, w_fd };
    if (copy_to_user(user_pipefd, k_fds, sizeof(int) * 2) < 0) {
        spin_lock(&curproc->p_lock);
        curproc->p_fd_table[r_fd] = NULL;
        curproc->p_fd_table[w_fd] = NULL;
        spin_unlock(&curproc->p_lock);
        file_close(rf);
        file_close(wf);
        return -EFAULT;
    }

    return 0;
}
