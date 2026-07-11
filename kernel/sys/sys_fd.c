#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/syscall.h>
#include <kernel/lock.h>
#include <kernel/clock.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>
#include <uapi/fcntl.h>
#include <uapi/sys/stat.h>
#include <string.h>
#include <kernel/fd.h>

// ==================================== eventfd
static ssize_t eventfd_read(struct vnode *vp, void *buf, size_t count, off_t off) {
    (void)off;
    struct eventfd_buffer *eb = (struct eventfd_buffer *)vp->data;
    if (!eb) return -EINVAL;
    if (count < sizeof(uint64_t)) return -EINVAL;

    spin_lock(&eb->lock);
    while (eb->counter == 0) {
        if (eb->flags & 00004000) { // EFD_NONBLOCK
            spin_unlock(&eb->lock);
            return -EAGAIN;
        }
        curthread->t_state = THREAD_WAITING;
        add_wait_queue(&eb->waitq, curthread);
        spin_unlock(&eb->lock);
        thread_yield();
        spin_lock(&eb->lock);
    }
    remove_wait_queue(&eb->waitq, curthread);

    uint64_t value;
    if (eb->flags & 1) { // EFD_SEMAPHORE
        value = 1;
        eb->counter--;
    } else {
        value = eb->counter;
        eb->counter = 0;
    }
    spin_unlock(&eb->lock);

    if (copy_to_user(buf, &value, sizeof(uint64_t)) < 0) {
        return -EFAULT;
    }
    return sizeof(uint64_t);
}

static ssize_t eventfd_write(struct vnode *vp, const void *buf, size_t count, off_t off) {
    (void)off;
    struct eventfd_buffer *eb = (struct eventfd_buffer *)vp->data;
    if (!eb) return -EINVAL;
    if (count < sizeof(uint64_t)) return -EINVAL;

    uint64_t value = 0;
    if (copy_from_user(&value, buf, sizeof(uint64_t)) < 0) {
        return -EFAULT;
    }

    if (value == 0xffffffffffffffffULL) {
        return -EINVAL;
    }

    spin_lock(&eb->lock);
    while (0xfffffffffffffffeULL - eb->counter < value) {
        if (eb->flags & 00004000) { // EFD_NONBLOCK
            spin_unlock(&eb->lock);
            return -EAGAIN;
        }
        curthread->t_state = THREAD_WAITING;
        add_wait_queue(&eb->waitq, curthread);
        spin_unlock(&eb->lock);
        thread_yield();
        spin_lock(&eb->lock);
    }
    remove_wait_queue(&eb->waitq, curthread);

    eb->counter += value;
    spin_unlock(&eb->lock);

    wake_up(&eb->waitq);
    return sizeof(uint64_t);
}

static int eventfd_inactive(struct vnode *vp) {
    struct eventfd_buffer *eb = (struct eventfd_buffer *)vp->data;
    if (eb) {
        spin_lock(&eb->lock);
        eb->refcnt--;
        int do_free = (eb->refcnt == 0);
        if (do_free)
            wake_up_all(&eb->waitq);
        spin_unlock(&eb->lock);
        if (do_free)
            kfree(eb);
    }
    vp->data = NULL;
    return 0;
}

static int eventfd_getattr(struct vnode *vp, struct stat *st) {
    memset(st, 0, sizeof(struct stat));
    st->st_mode = S_IFIFO | 0666;
    st->st_nlink = 1;
    struct eventfd_buffer *eb = (struct eventfd_buffer *)vp->data;
    if (eb) {
        st->st_size = sizeof(uint64_t);
    }
    return 0;
}

struct vnode_ops eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .inactive = eventfd_inactive,
    .getattr = eventfd_getattr,
};

int64_t sys_eventfd2(unsigned int initval, int flags) {
    struct eventfd_buffer *eb = kmalloc(sizeof(struct eventfd_buffer));
    if (!eb) return -ENOMEM;
    memset(eb, 0, sizeof(struct eventfd_buffer));
    eb->counter = initval;
    eb->flags = flags;
    eb->refcnt = 1;
    spin_lock_init(&eb->lock);
    init_waitqueue_head(&eb->waitq);

    struct vnode *vp = vnode_alloc(S_IFIFO, &eventfd_ops);
    if (!vp) {
        kfree(eb);
        return -ENOMEM;
    }
    vp->data = eb;

    struct file *f = file_alloc();
    if (!f) {
        vput(vp);
        return -ENOMEM;
    }
    f->f_vn = vp;
    f->f_flags = O_RDWR;
    if (flags & 00004000) { // EFD_NONBLOCK
        f->f_flags |= O_NONBLOCK;
    }

    int fd = proc_alloc_fd(curproc, f);
    if (fd < 0) {
        file_close(f);
        return fd;
    }
    file_close(f);

    return fd;
}

int64_t sys_eventfd(unsigned int initval) {
    return sys_eventfd2(initval, 0);
}


// ==================================== signalfd

struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_type;
    uint8_t  __pad[48];
};

static ssize_t signalfd_read(struct vnode *vp, void *buf, size_t count, off_t off) {
    (void)off;
    struct signalfd_buffer *sb = (struct signalfd_buffer *)vp->data;
    if (!sb) return -EINVAL;
    if (count < sizeof(struct signalfd_siginfo)) return -EINVAL;

    while (1) {
        spin_lock(&sb->lock);
        uint32_t signo = 0;
        struct proc *p = curproc;
        uint64_t pflags = spin_lock_irqsave(&p->p_lock);
        uint64_t pending = curthread->t_sig_pending & sb->mask;
        if (pending != 0) {
            for (int i = 1; i <= 64; i++) {
                if (pending & (1ULL << (i - 1))) {
                    signo = i;
                    curthread->t_sig_pending &= ~(1ULL << (i - 1));
                    break;
                }
            }
        }
        spin_unlock_irqrestore(&p->p_lock, pflags);
        spin_unlock(&sb->lock);

        if (signo > 0) {
            struct signalfd_siginfo sinfo;
            memset(&sinfo, 0, sizeof(sinfo));
            sinfo.ssi_signo = signo;
            sinfo.ssi_code = -1; // SI_USER
            if (copy_to_user(buf, &sinfo, sizeof(sinfo)) < 0) {
                return -EFAULT;
            }
            return sizeof(sinfo);
        }

        if (sb->flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        thread_yield();
    }
}

static ssize_t signalfd_write(struct vnode *vp, const void *buf, size_t count, off_t off) {
    (void)vp; (void)buf; (void)count; (void)off;
    return -EINVAL;
}

static int signalfd_inactive(struct vnode *vp) {
    struct signalfd_buffer *sb = (struct signalfd_buffer *)vp->data;
    if (sb) {
        spin_lock(&sb->lock);
        sb->refcnt--;
        if (sb->refcnt == 0) {
            spin_unlock(&sb->lock);
            kfree(sb);
        } else {
            spin_unlock(&sb->lock);
        }
    }
    vp->data = NULL;
    return 0;
}

static int signalfd_getattr(struct vnode *vp, struct stat *st) {
    memset(st, 0, sizeof(struct stat));
    st->st_mode = S_IFIFO | 0666;
    st->st_nlink = 1;
    struct signalfd_buffer *sb = (struct signalfd_buffer *)vp->data;
    if (sb) {
        st->st_size = sizeof(struct signalfd_siginfo);
    }
    return 0;
}

struct vnode_ops signalfd_ops = {
    .read = signalfd_read,
    .write = signalfd_write,
    .inactive = signalfd_inactive,
    .getattr = signalfd_getattr,
};

int64_t sys_signalfd4(int fd, const sigset_t *user_mask, size_t sizemask, int flags) {
    if (sizemask != sizeof(sigset_t)) {
        return -EINVAL;
    }
    sigset_t mask;
    if (copy_from_user(&mask, user_mask, sizeof(sigset_t)) < 0) {
        return -EFAULT;
    }

    if (fd == -1) {
        struct signalfd_buffer *sb = kmalloc(sizeof(struct signalfd_buffer));
        if (!sb) return -ENOMEM;
        memset(sb, 0, sizeof(struct signalfd_buffer));
        sb->mask = mask;
        sb->flags = flags;
        sb->refcnt = 1;
        spin_lock_init(&sb->lock);

        struct vnode *vp = vnode_alloc(S_IFIFO, &signalfd_ops);
        if (!vp) {
            kfree(sb);
            return -ENOMEM;
        }
        vp->data = sb;

        struct file *f = file_alloc();
        if (!f) {
            vput(vp);
            return -ENOMEM;
        }
        f->f_vn = vp;
        f->f_flags = O_RDONLY;
        if (flags & O_NONBLOCK) {
            f->f_flags |= O_NONBLOCK;
        }

        int new_fd = proc_alloc_fd(curproc, f);
        if (new_fd < 0) {
            file_close(f);
            return new_fd;
        }
        file_close(f);
        return new_fd;
    } else {
        struct file *f = fdget(fd);
        if (!f) return -EBADF;
        if (!f->f_vn || f->f_vn->ops != &signalfd_ops) { fdput(f); return -EINVAL; }

        struct signalfd_buffer *sb = (struct signalfd_buffer *)f->f_vn->data;
        if (!sb) { fdput(f); return -EINVAL; }

        spin_lock(&sb->lock);
        sb->mask = mask;
        sb->flags = flags;
        spin_unlock(&sb->lock);

        fdput(f);
        return fd;
    }
}

int64_t sys_signalfd(int fd, const sigset_t *user_mask, size_t sizemask) {
    return sys_signalfd4(fd, user_mask, sizemask, 0);
}


// ==================================== timerfd

void timerfd_update_ticks(struct timerfd_buffer *tb, uint64_t now_ns) {
    if (tb->expire_time_ns == 0) {
        return;
    }

    if (now_ns >= tb->expire_time_ns) {
        if (tb->interval_ns == 0) {
            tb->ticks_count += 1;
            tb->expire_time_ns = 0;
        } else {
            uint64_t elapsed_ns = now_ns - tb->expire_time_ns;
            uint64_t missed_ticks = 1 + elapsed_ns / tb->interval_ns;
            tb->ticks_count += missed_ticks;
            tb->expire_time_ns += missed_ticks * tb->interval_ns;
        }
    }
}

static ssize_t timerfd_read(struct vnode *vp, void *buf, size_t count, off_t off) {
    (void)off;
    struct timerfd_buffer *tb = (struct timerfd_buffer *)vp->data;
    if (!tb) return -EINVAL;
    if (count < sizeof(uint64_t)) return -EINVAL;

    while (1) {
        spin_lock(&tb->lock);
        timerfd_update_ticks(tb, get_uptime_ns());
        uint64_t ticks = tb->ticks_count;
        if (ticks > 0) {
            tb->ticks_count = 0;
            spin_unlock(&tb->lock);

            if (copy_to_user(buf, &ticks, sizeof(uint64_t)) < 0) {
                return -EFAULT;
            }
            return sizeof(uint64_t);
        }
        spin_unlock(&tb->lock);

        if (tb->flags & 00004000) { // O_NONBLOCK / TFD_NONBLOCK
            return -EAGAIN;
        }
        thread_yield();
    }
}

static ssize_t timerfd_write(struct vnode *vp, const void *buf, size_t count, off_t off) {
    (void)vp; (void)buf; (void)count; (void)off;
    return -EINVAL;
}

static int timerfd_inactive(struct vnode *vp) {
    struct timerfd_buffer *tb = (struct timerfd_buffer *)vp->data;
    if (tb) {
        spin_lock(&tb->lock);
        tb->refcnt--;
        if (tb->refcnt == 0) {
            spin_unlock(&tb->lock);
            kfree(tb);
        } else {
            spin_unlock(&tb->lock);
        }
    }
    vp->data = NULL;
    return 0;
}

static int timerfd_getattr(struct vnode *vp, struct stat *st) {
    memset(st, 0, sizeof(struct stat));
    st->st_mode = S_IFIFO | 0666;
    st->st_nlink = 1;
    struct timerfd_buffer *tb = (struct timerfd_buffer *)vp->data;
    if (tb) {
        st->st_size = sizeof(uint64_t);
    }
    return 0;
}

struct vnode_ops timerfd_ops = {
    .read = timerfd_read,
    .write = timerfd_write,
    .inactive = timerfd_inactive,
    .getattr = timerfd_getattr,
};

int64_t sys_timerfd_create(int clockid, int flags) {
    if (clockid != 0 && clockid != 1) {
        return -EINVAL;
    }

    struct timerfd_buffer *tb = kmalloc(sizeof(struct timerfd_buffer));
    if (!tb) return -ENOMEM;
    memset(tb, 0, sizeof(struct timerfd_buffer));
    tb->clockid = clockid;
    tb->flags = flags;
    tb->refcnt = 1;
    spin_lock_init(&tb->lock);

    struct vnode *vp = vnode_alloc(S_IFIFO, &timerfd_ops);
    if (!vp) {
        kfree(tb);
        return -ENOMEM;
    }
    vp->data = tb;

    struct file *f = file_alloc();
    if (!f) {
        vput(vp);
        return -ENOMEM;
    }
    f->f_vn = vp;
    f->f_flags = O_RDONLY;
    if (flags & 00004000) { // O_NONBLOCK / TFD_NONBLOCK
        f->f_flags |= O_NONBLOCK;
    }

    int fd = proc_alloc_fd(curproc, f);
    if (fd < 0) {
        file_close(f);
        return fd;
    }
    file_close(f);
    return fd;
}

int64_t sys_timerfd_settime(int fd, int flags, const struct itimerspec *user_new_value, struct itimerspec *user_old_value) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    if (!f->f_vn || f->f_vn->ops != &timerfd_ops) { fdput(f); return -EINVAL; }

    struct timerfd_buffer *tb = (struct timerfd_buffer *)f->f_vn->data;
    if (!tb) { fdput(f); return -EINVAL; }

    struct itimerspec new_val;
    if (!is_user_address_range(user_new_value, sizeof(struct itimerspec))) { fdput(f); return -EFAULT; }
    if (copy_from_user(&new_val, user_new_value, sizeof(struct itimerspec)) < 0) { fdput(f); return -EFAULT; }

    spin_lock(&tb->lock);

    uint64_t now_ns = get_uptime_ns();
    timerfd_update_ticks(tb, now_ns);

    if (user_old_value) {
        struct itimerspec old_val = tb->value;
        spin_unlock(&tb->lock);
        if (!is_user_address_range(user_old_value, sizeof(struct itimerspec))) { fdput(f); return -EFAULT; }
        if (copy_to_user(user_old_value, &old_val, sizeof(struct itimerspec)) < 0) { fdput(f); return -EFAULT; }
        spin_lock(&tb->lock);
    }

    tb->value = new_val;
    tb->ticks_count = 0;

    uint64_t it_val_ns = (uint64_t)new_val.it_value.tv_sec * 1000000000ULL + new_val.it_value.tv_nsec;
    uint64_t it_int_ns = (uint64_t)new_val.it_interval.tv_sec * 1000000000ULL + new_val.it_interval.tv_nsec;

    if (it_val_ns == 0) {
        tb->expire_time_ns = 0;
        tb->interval_ns = 0;
    } else {
        tb->interval_ns = it_int_ns;
        if (flags & 1) { // TFD_TIMER_ABSTIME
            tb->expire_time_ns = it_val_ns;
        } else {
            tb->expire_time_ns = now_ns + it_val_ns;
        }
        tb->start_time_ns = now_ns;
    }

    spin_unlock(&tb->lock);
    fdput(f);
    return 0;
}

int64_t sys_timerfd_gettime(int fd, struct itimerspec *user_curr_value) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    if (!f->f_vn || f->f_vn->ops != &timerfd_ops) { fdput(f); return -EINVAL; }

    struct timerfd_buffer *tb = (struct timerfd_buffer *)f->f_vn->data;
    if (!tb) { fdput(f); return -EINVAL; }

    spin_lock(&tb->lock);
    uint64_t now_ns = get_uptime_ns();
    timerfd_update_ticks(tb, now_ns);

    struct itimerspec curr_val;
    curr_val.it_interval = tb->value.it_interval;

    if (tb->expire_time_ns == 0) {
        curr_val.it_value.tv_sec = 0;
        curr_val.it_value.tv_nsec = 0;
    } else {
        uint64_t rem_ns = 0;
        if (tb->expire_time_ns > now_ns) {
            rem_ns = tb->expire_time_ns - now_ns;
        }
        curr_val.it_value.tv_sec = rem_ns / 1000000000ULL;
        curr_val.it_value.tv_nsec = rem_ns % 1000000000ULL;
    }
    spin_unlock(&tb->lock);

    if (!is_user_address_range(user_curr_value, sizeof(struct itimerspec))) { fdput(f); return -EFAULT; }
    if (copy_to_user(user_curr_value, &curr_val, sizeof(struct itimerspec)) < 0) { fdput(f); return -EFAULT; }
    fdput(f);
    return 0;
}
