#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/syscall.h>
#include <kernel/lock.h>
#include <kernel/clock.h>
#include <kernel/drm.h>
#include <kernel/socket.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>
#include <kernel/fd.h>

#include <uapi/errno.h>
#include <uapi/fcntl.h>

#include <string.h>


#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLONESHOT 0x40000000

extern struct vnode_ops unix_socket_ops;

uint32_t check_fd_readiness(int fd, uint32_t events) {
    struct file *f = fdget(fd);
    if (!f) return POLLNVAL;

    uint32_t revents = 0;
    
    if (f->f_vn && f->f_vn->ops == &unix_socket_ops) {
        struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
        if (s) {
            spin_lock(&s->lock);
            if (s->state == SS_LISTENING) {
                if (s->backlog_head != s->backlog_tail) {
                    revents |= POLLIN;
                }
            } else if (s->state == SS_CONNECTED) {
                if (s->buf_head != s->buf_tail) {
                    revents |= POLLIN;
                }
                uint32_t next_tail = s->peer ? (s->peer->buf_tail + 1) % UNIX_SOCKET_BUF_SIZE : 0;
                if (s->peer && next_tail != s->peer->buf_head) {
                    revents |= POLLOUT;
                }
            } else if (s->state == SS_DISCONNECTED) {
                revents |= (POLLHUP | POLLIN);
            }
            spin_unlock(&s->lock);
        }
    } else if (f->f_vn && f->f_vn->ops == &eventfd_ops) {
        struct eventfd_buffer *eb = (struct eventfd_buffer *)f->f_vn->data;
        if (eb) {
            spin_lock(&eb->lock);
            if (eb->counter > 0) {
                revents |= POLLIN;
            }
            if (eb->counter < 0xfffffffffffffffeULL) {
                revents |= POLLOUT;
            }
            spin_unlock(&eb->lock);
        }
    } else if (f->f_vn && f->f_vn->ops == &signalfd_ops) {
        struct signalfd_buffer *sb = (struct signalfd_buffer *)f->f_vn->data;
        if (sb) {
            spin_lock(&sb->lock);
            if (curthread->t_sig_pending & sb->mask) {
                revents |= POLLIN;
            }
            spin_unlock(&sb->lock);
        }
    } else if (f->f_vn && f->f_vn->ops == &timerfd_ops) {
        struct timerfd_buffer *tb = (struct timerfd_buffer *)f->f_vn->data;
        if (tb) {
            spin_lock(&tb->lock);
            timerfd_update_ticks(tb, get_uptime_ns());
            if (tb->ticks_count > 0) {
                revents |= POLLIN;
            }
            spin_unlock(&tb->lock);
        }
    } else if (f->f_vn && strcmp(f->f_vn->v_name, "card0") == 0) {
        if (has_drm_event()) {
            revents |= POLLIN;
        }
        revents |= (events & POLLOUT);
    } else if (f->f_vn && (strcmp(f->f_vn->v_name, "kbd") == 0 || strcmp(f->f_vn->v_name, "tty") == 0)) {
        extern bool keyboard_has_data(void);
        if (keyboard_has_data()) {
            revents |= POLLIN;
        }
        revents |= (events & POLLOUT);
    } else {
        revents |= (events & (POLLIN | POLLOUT));
    }

    fdput(f);
    return revents;
}

#define MAX_EPOLL_ITEMS 128

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct epoll_item {
    int fd;
    struct epoll_event event;
};

struct epoll_instance {
    struct epoll_item items[MAX_EPOLL_ITEMS];
    int count;
    spinlock_t lock;
};

static int epoll_inactive(struct vnode *vp) {
    struct epoll_instance *ei = (struct epoll_instance *)vp->data;
    if (ei) {
        kfree(ei);
    }
    vp->data = NULL;
    return 0;
}

static struct vnode_ops epoll_ops = {
    .inactive = epoll_inactive,
};

int64_t sys_epoll_create1(int flags) {
    (void)flags;
    struct epoll_instance *ei = kmalloc(sizeof(struct epoll_instance));
    if (!ei) return -ENOMEM;
    memset(ei, 0, sizeof(struct epoll_instance));
    spin_lock_init(&ei->lock);

    struct vnode *vp = vnode_alloc(S_IFCHR, &epoll_ops);
    if (!vp) {
        kfree(ei);
        return -ENOMEM;
    }
    vp->data = ei;

    struct file *f = file_alloc();
    if (!f) {
        vput(vp);
        return -ENOMEM;
    }
    f->f_vn = vp;
    f->f_flags = O_RDWR;

    int fd = proc_alloc_fd(curproc, f);
    if (fd < 0) {
        file_close(f);
        return fd;
    }
    file_close(f);

    return fd;
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, void *user_event) {
    struct file *epf = fdget(epfd);
    if (!epf) return -EBADF;
    if (!epf->f_vn || epf->f_vn->ops != &epoll_ops) { fdput(epf); return -EINVAL; }

    struct file *target_f = fdget(fd);
    if (!target_f) { fdput(epf); return -EBADF; }

    struct epoll_instance *ei = (struct epoll_instance *)epf->f_vn->data;
    if (!ei) { fdput(epf); fdput(target_f); return -EINVAL; }

    struct epoll_event ev;
    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (!is_user_address_range(user_event, sizeof(struct epoll_event))) { fdput(epf); fdput(target_f); return -EFAULT; }
        if (copy_from_user(&ev, user_event, sizeof(struct epoll_event)) < 0) { fdput(epf); fdput(target_f); return -EFAULT; }
    }

    uint64_t irq_flags = spin_lock_irqsave(&ei->lock);

    if (op == EPOLL_CTL_ADD) {
        for (int i = 0; i < ei->count; i++) {
            if (ei->items[i].fd == fd) {
                spin_unlock_irqrestore(&ei->lock, irq_flags);
                fdput(epf); fdput(target_f);
                return -EEXIST;
            }
        }
        if (ei->count >= MAX_EPOLL_ITEMS) {
            spin_unlock_irqrestore(&ei->lock, irq_flags);
            fdput(epf); fdput(target_f);
            return -ENOMEM;
        }
        ei->items[ei->count].fd = fd;
        ei->items[ei->count].event = ev;
        ei->count++;
    } else if (op == EPOLL_CTL_MOD) {
        int found = -1;
        for (int i = 0; i < ei->count; i++) {
            if (ei->items[i].fd == fd) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            spin_unlock_irqrestore(&ei->lock, irq_flags);
            fdput(epf); fdput(target_f);
            return -ENOENT;
        }
        ei->items[found].event = ev;
    } else if (op == EPOLL_CTL_DEL) {
        int found = -1;
        for (int i = 0; i < ei->count; i++) {
            if (ei->items[i].fd == fd) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            spin_unlock_irqrestore(&ei->lock, irq_flags);
            fdput(epf); fdput(target_f);
            return -ENOENT;
        }
        for (int i = found; i < ei->count - 1; i++) {
            ei->items[i] = ei->items[i+1];
        }
        ei->count--;
    } else {
        spin_unlock_irqrestore(&ei->lock, irq_flags);
        fdput(epf); fdput(target_f);
        return -EINVAL;
    }

    spin_unlock_irqrestore(&ei->lock, irq_flags);
    fdput(epf); fdput(target_f);
    return 0;
}

int64_t sys_epoll_wait(int epfd, void *user_events, int maxevents, int timeout) {
    if (maxevents <= 0) return -EINVAL;
    if ((size_t)maxevents > (SIZE_MAX / sizeof(struct epoll_event))) return -EINVAL;
    if (!is_user_address_range(user_events, sizeof(struct epoll_event) * maxevents)) return -EFAULT;

    struct file *epf = fdget(epfd);
    if (!epf || !epf->f_vn || epf->f_vn->ops != &epoll_ops) { fdput(epf); return -EINVAL; }

    struct epoll_instance *ei = (struct epoll_instance *)epf->f_vn->data;
    if (!ei) { fdput(epf); return -EINVAL; }

    struct epoll_event *events = kmalloc(sizeof(struct epoll_event) * maxevents);
    if (!events) { fdput(epf); return -ENOMEM; }

    int ready_count = 0;
    uint64_t deadline = (timeout > 0) ? get_uptime_ns() + (uint64_t)timeout * 1000000 : 0;
    while (1) {
        ready_count = 0;

        uint64_t irq_flags = spin_lock_irqsave(&ei->lock);

        for (int i = 0; i < ei->count && ready_count < maxevents; i++) {
            int fd = ei->items[i].fd;
            uint32_t req_events = ei->items[i].event.events;
            uint32_t revents = check_fd_readiness(fd, req_events);

            if (revents & req_events) {
                events[ready_count].events = revents & req_events;
                events[ready_count].data = ei->items[i].event.data;
                if (ei->items[i].event.events & EPOLLONESHOT)
                    ei->items[i].event.events = 0;
                ready_count++;
            }
        }

        spin_unlock_irqrestore(&ei->lock, irq_flags);

        if (ready_count > 0 || timeout == 0) {
            break;
        }

        thread_yield();

        if (timeout > 0 && get_uptime_ns() >= deadline) {
            break;
        }
    }

    if (ready_count > 0) {
        if (copy_to_user(user_events, events, sizeof(struct epoll_event) * ready_count) < 0) {
            kfree(events);
            fdput(epf);
            return -EFAULT;
        }
    }

    kfree(events);
    fdput(epf);
    return ready_count;
}

int64_t sys_epoll_pwait(int epfd, void *user_events, int maxevents, int timeout, void *user_sigmask) {
    (void)user_sigmask;
    return sys_epoll_wait(epfd, user_events, maxevents, timeout);
}
