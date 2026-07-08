#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/kmem.h>
#include <kernel/cpu.h>
#include <kernel/lock.h>
#include <kernel/drm.h>
#include <kernel/socket.h>
#include <kernel/fs/vnode.h>
#include <kernel/fs/file.h>

#include <uapi/errno.h>

#include <string.h>

extern struct vnode_ops pipe_ops;
extern struct vnode_ops unix_socket_ops;
extern struct vnode_ops netlink_socket_ops;

#define PIPE_BUF_SIZE 4096
struct pipe_buffer {
    char       buf[PIPE_BUF_SIZE];
    size_t     head;
    size_t     tail;
    size_t     size;
    spinlock_t lock;
    int        refcnt;
};

#define POLLIN     0x0001
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLPRI    0x0040

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

static void poll_check_fd(struct pollfd *fds, int i) {
    int fd = fds[i].fd;
    struct file *f = fdget(fd);
    if (!f) {
        fds[i].revents |= POLLNVAL;
        return;
    }

    if (f->f_vn && f->f_vn->ops == &unix_socket_ops) {
        struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
        if (s) {
            spin_lock(&s->lock);
            if (s->state == SS_LISTENING) {
                if (s->backlog_head != s->backlog_tail)
                    fds[i].revents |= POLLIN;
            } else if (s->state == SS_CONNECTED) {
                if (s->buf_head != s->buf_tail)
                    fds[i].revents |= POLLIN;
                uint32_t next_tail = s->peer ? (s->peer->buf_tail + 1) % UNIX_SOCKET_BUF_SIZE : 0;
                if (s->peer && next_tail != s->peer->buf_head)
                    fds[i].revents |= POLLOUT;
            } else if (s->state == SS_DISCONNECTED) {
                fds[i].revents |= (POLLHUP | POLLIN);
            }
            spin_unlock(&s->lock);
        }
    } else if (f->f_vn && f->f_vn->ops == &netlink_socket_ops) {
        /* netlink not implemented */
    } else if (f->f_vn && strcmp(f->f_vn->v_name, "card0") == 0) {
        if (has_drm_event())
            fds[i].revents |= POLLIN;
    } else if (f->f_vn && f->f_vn->ops == &pipe_ops) {
        struct pipe_buffer *pb = (struct pipe_buffer *)f->f_vn->data;
        if (pb) {
            if (pb->size > 0)
                fds[i].revents |= POLLIN;
            if (pb->refcnt >= 2 && PIPE_BUF_SIZE - pb->size > 0)
                fds[i].revents |= POLLOUT;
        }
    } else {
        fds[i].revents |= (fds[i].events & (POLLIN | POLLOUT));
    }

    if (fds[i].revents & fds[i].events)
        fds[i].revents = fds[i].revents & fds[i].events;
    fdput(f);
}

static int do_poll(struct pollfd *fds, int nfds, int timeout_ms) {
    int ready_count = 0;
    uint64_t deadline_ticks = (timeout_ms > 0) ? arch_get_system_ticks() + timeout_ms : 0;
    while (1) {
        ready_count = 0;
        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            poll_check_fd(fds, i);
            if (fds[i].revents & fds[i].events)
                ready_count++;
        }
        if (ready_count > 0 || timeout_ms == 0) break;
        thread_yield();
        if (timeout_ms > 0 && arch_get_system_ticks() >= deadline_ticks) break;
    }
    return ready_count;
}

int64_t sys_poll(void *user_fds, uint64_t nfds, int timeout) {
    if (nfds == 0) return 0;
    if (!is_user_address_range(user_fds, sizeof(struct pollfd) * nfds)) return -EFAULT;

    struct pollfd *fds = kmalloc(sizeof(struct pollfd) * nfds);
    if (!fds) return -ENOMEM;

    if (copy_from_user(fds, user_fds, sizeof(struct pollfd) * nfds) < 0) {
        kfree(fds);
        return -EFAULT;
    }

    int ready_count = do_poll(fds, (int)nfds, timeout);

    if (copy_to_user(user_fds, fds, sizeof(struct pollfd) * nfds) < 0) {
        kfree(fds);
        return -EFAULT;
    }

    kfree(fds);
    return ready_count;
}

#define FD_SET_BITS 1024
#define FD_SET_BYTES (FD_SET_BITS / 8)

static inline int fd_isset(const uint8_t *set, int fd) {
    return set[fd / 8] & (1 << (fd % 8));
}
static inline void fd_set_bit(uint8_t *set, int fd) {
    set[fd / 8] |= (1 << (fd % 8));
}

int64_t sys_select(int nfds, void *user_readfds, void *user_writefds,
                   void *user_exceptfds, void *user_timeout, uint64_t sigsetsize) {
    (void)sigsetsize;
    if (nfds < 0 || nfds > FD_SET_BITS) return -EINVAL;

    uint8_t read_set[FD_SET_BYTES] = {0};
    uint8_t write_set[FD_SET_BYTES] = {0};
    uint8_t except_set[FD_SET_BYTES] = {0};

    if (user_readfds && copy_from_user(read_set, user_readfds, FD_SET_BYTES) < 0)
        return -EFAULT;
    if (user_writefds && copy_from_user(write_set, user_writefds, FD_SET_BYTES) < 0)
        return -EFAULT;
    if (user_exceptfds && copy_from_user(except_set, user_exceptfds, FD_SET_BYTES) < 0)
        return -EFAULT;

    int poll_count = 0;
    for (int fd = 0; fd < nfds; fd++) {
        if ((user_readfds && fd_isset(read_set, fd)) ||
            (user_writefds && fd_isset(write_set, fd)) ||
            (user_exceptfds && fd_isset(except_set, fd)))
            poll_count++;
    }

    struct pollfd *fds = kmalloc(sizeof(struct pollfd) * (poll_count + 1));
    if (!fds) return -ENOMEM;

    int idx = 0;
    for (int fd = 0; fd < nfds; fd++) {
        uint32_t events = 0;
        if (user_readfds && fd_isset(read_set, fd)) events |= POLLIN;
        if (user_writefds && fd_isset(write_set, fd)) events |= POLLOUT;
        if (user_exceptfds && fd_isset(except_set, fd)) events |= POLLPRI;
        if (events) {
            fds[idx].fd = fd;
            fds[idx].events = events;
            fds[idx].revents = 0;
            idx++;
        }
    }

    int timeout_ms = -1;
    if (user_timeout) {
        struct { int64_t sec; int64_t usec; } tv;
        if (copy_from_user(&tv, user_timeout, sizeof(tv)) < 0) {
            kfree(fds);
            return -EFAULT;
        }
        timeout_ms = (int)(tv.sec * 1000 + tv.usec / 1000);
        if (timeout_ms < 0) timeout_ms = 0;
    }

    do_poll(fds, poll_count, timeout_ms);

    uint8_t read_result[FD_SET_BYTES] = {0};
    uint8_t write_result[FD_SET_BYTES] = {0};
    uint8_t except_result[FD_SET_BYTES] = {0};
    int actual_ready = 0;

    for (int i = 0; i < poll_count; i++) {
        int fd = fds[i].fd;
        int ready = 0;
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            fd_set_bit(read_result, fd);
            ready = 1;
        }
        if (fds[i].revents & POLLOUT) {
            fd_set_bit(write_result, fd);
            ready = 1;
        }
        if (fds[i].revents & POLLPRI) {
            fd_set_bit(except_result, fd);
            ready = 1;
        }
        if (ready) actual_ready++;
    }

    if (user_readfds && copy_to_user(user_readfds, read_result, FD_SET_BYTES) < 0) {
        kfree(fds);
        return -EFAULT;
    }
    if (user_writefds && copy_to_user(user_writefds, write_result, FD_SET_BYTES) < 0) {
        kfree(fds);
        return -EFAULT;
    }
    if (user_exceptfds && copy_to_user(user_exceptfds, except_result, FD_SET_BYTES) < 0) {
        kfree(fds);
        return -EFAULT;
    }

    kfree(fds);
    return actual_ready;
}
