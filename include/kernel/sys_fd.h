#pragma once

#include <stdint.h>
#include <kernel/lock.h>
#include <uapi/types.h>

struct eventfd_buffer {
    uint64_t counter;
    spinlock_t lock;
    int flags;
    int refcnt;
};

struct signalfd_buffer {
    sigset_t mask;
    int flags;
    spinlock_t lock;
    int refcnt;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

struct timerfd_buffer {
    int clockid;
    int flags;
    struct itimerspec value;
    uint64_t start_time_ns;
    uint64_t interval_ns;
    uint64_t expire_time_ns;
    uint64_t ticks_count;
    spinlock_t lock;
    int refcnt;
};

extern struct vnode_ops eventfd_ops;
extern struct vnode_ops signalfd_ops;
extern struct vnode_ops timerfd_ops;

void timerfd_update_ticks(struct timerfd_buffer *tb, uint64_t now_ns);
