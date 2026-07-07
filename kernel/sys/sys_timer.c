#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <kernel/fd.h>

#include <string.h>

extern spinlock_t g_proc_list_lock;
extern struct list_node g_proc_list;

union sigval_kernel {
    int sival_int;
    void *sival_ptr;
};

struct sigevent_kernel {
    union sigval_kernel sigev_value;
    int sigev_signo;
    int sigev_notify;
};

extern uint64_t get_uptime_ns(void);

int64_t sys_timer_create(int clockid, const void *sevp_user, int *timerid_user) {
    dprintf("[KERNEL sys_timer_create] clockid=%d, sevp=%p, timerid=%p\n", clockid, sevp_user, timerid_user);
    if (clockid != 0 && clockid != 1) { // CLOCK_REALTIME or CLOCK_MONOTONIC
        return -EINVAL;
    }

    struct thread *t = curthread;
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (!t->t_timers[i].pt_used) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        dprintf("[KERNEL sys_timer_create] Failed: no slots available\n");
        return -EAGAIN;
    }

    int sig = 14; // SIGALRM
    if (sevp_user) {
        if (!is_user_address_range(sevp_user, sizeof(struct sigevent_kernel))) {
            return -EFAULT;
        }
        struct sigevent_kernel sev;
        if (copy_from_user(&sev, sevp_user, sizeof(struct sigevent_kernel)) < 0) {
            return -EFAULT;
        }
        if (sev.sigev_signo >= 1 && sev.sigev_signo < NSIG) {
            sig = sev.sigev_signo;
        }
    }

    t->t_timers[slot].pt_used = true;
    t->t_timers[slot].pt_clock_id = clockid;
    t->t_timers[slot].pt_sig_no = sig;
    t->t_timers[slot].pt_interval_ns = 0;
    t->t_timers[slot].pt_value_ns = 0;

    if (!is_user_address_range(timerid_user, sizeof(int))) {
        t->t_timers[slot].pt_used = false;
        return -EFAULT;
    }

    if (copy_to_user(timerid_user, &slot, sizeof(int)) < 0) {
        t->t_timers[slot].pt_used = false;
        return -EFAULT;
    }

    dprintf("[KERNEL sys_timer_create] Succeeded: assigned slot %d, signal %d\n", slot, sig);
    return 0;
}

int64_t sys_timer_settime(int timerid, int flags, const struct itimerspec *new_value_user, struct itimerspec *old_value_user) {
    dprintf("[KERNEL sys_timer_settime] timerid=%d, flags=%d, new_value=%p, old_value=%p\n", timerid, flags, new_value_user, old_value_user);
    if (timerid < 0 || timerid >= 8) {
        return -EINVAL;
    }

    struct thread *t = curthread;
    struct posix_timer *timer = &t->t_timers[timerid];
    if (!timer->pt_used) {
        return -EINVAL;
    }

    uint64_t now = get_uptime_ns();

    if (old_value_user) {
        if (!is_user_address_range(old_value_user, sizeof(struct itimerspec))) {
            return -EFAULT;
        }
        struct itimerspec old_val;
        old_val.it_interval.tv_sec = timer->pt_interval_ns / 1000000000ULL;
        old_val.it_interval.tv_nsec = timer->pt_interval_ns % 1000000000ULL;

        if (timer->pt_value_ns > 0 && timer->pt_value_ns > now) {
            uint64_t diff = timer->pt_value_ns - now;
            old_val.it_value.tv_sec = diff / 1000000000ULL;
            old_val.it_value.tv_nsec = diff % 1000000000ULL;
        } else {
            old_val.it_value.tv_sec = 0;
            old_val.it_value.tv_nsec = 0;
        }

        if (copy_to_user(old_value_user, &old_val, sizeof(struct itimerspec)) < 0) {
            return -EFAULT;
        }
    }

    if (!new_value_user) {
        return -EINVAL;
    }

    if (!is_user_address_range(new_value_user, sizeof(struct itimerspec))) {
        return -EFAULT;
    }

    struct itimerspec new_val;
    if (copy_from_user(&new_val, new_value_user, sizeof(struct itimerspec)) < 0) {
        return -EFAULT;
    }

    if (new_val.it_value.tv_sec < 0 || new_val.it_value.tv_nsec < 0 || new_val.it_value.tv_nsec >= 1000000000LL) {
        return -EINVAL;
    }
    if (new_val.it_interval.tv_sec < 0 || new_val.it_interval.tv_nsec < 0 || new_val.it_interval.tv_nsec >= 1000000000LL) {
        return -EINVAL;
    }

    uint64_t it_value_ns = new_val.it_value.tv_sec * 1000000000ULL + new_val.it_value.tv_nsec;
    uint64_t it_interval_ns = new_val.it_interval.tv_sec * 1000000000ULL + new_val.it_interval.tv_nsec;

    if (it_value_ns == 0) {
        // disarm
        timer->pt_value_ns = 0;
        timer->pt_interval_ns = 0;
        dprintf("[KERNEL sys_timer_settime] Disarmed timer %d\n", timerid);
        return 0;
    }

    timer->pt_interval_ns = it_interval_ns;
    if ((flags & 1) != 0) { // TIMER_ABSTIME
        timer->pt_value_ns = it_value_ns;
    } else {
        timer->pt_value_ns = now + it_value_ns;
    }

    dprintf("[KERNEL sys_timer_settime] Armed timer %d: value=%lld ns, interval=%lld ns\n", timerid, timer->pt_value_ns, timer->pt_interval_ns);
    return 0;
}

int64_t sys_timer_delete(int timerid) {
    dprintf("[KERNEL sys_timer_delete] timerid=%d\n", timerid);
    if (timerid < 0 || timerid >= 8) {
        return -EINVAL;
    }

    struct thread *t = curthread;
    struct posix_timer *timer = &t->t_timers[timerid];
    if (!timer->pt_used) {
        return -EINVAL;
    }

    timer->pt_used = false;
    timer->pt_value_ns = 0;
    timer->pt_interval_ns = 0;
    return 0;
}

void posix_timers_tick(void) {
    uint64_t now = get_uptime_ns();

    uint64_t flags = spin_lock_irqsave(&g_proc_list_lock);

    struct proc *p;
    list_for_each_entry(p, &g_proc_list, p_list_link) {
        if(!spin_trylock(&p->p_lock)) continue;

        struct thread *t = p->p_threads;
        while (t) {
            for (int i = 0; i < 8; i++) {
                struct posix_timer *timer = &t->t_timers[i];
                if (timer->pt_used && timer->pt_value_ns > 0) {
                    if (now >= timer->pt_value_ns) {
                        int sig = timer->pt_sig_no;
                        dprintf("[KERNEL POSIX TIMER EXPIRED] thread=%p, slot=%d, sig=%d, now=%lld, val=%lld\n", t, i, sig, now, timer->pt_value_ns);
                        if (sig >= 1 && sig < NSIG) {
                            t->t_sig_pending |= (1ULL << (sig - 1));
                            thread_signal_wakeup(t);
                        }

                        if (timer->pt_interval_ns > 0) {
                            timer->pt_value_ns += timer->pt_interval_ns;
                            if (timer->pt_value_ns <= now) {
                                timer->pt_value_ns = now + timer->pt_interval_ns;
                            }
                        } else {
                            timer->pt_value_ns = 0;
                        }
                    }
                }
            }
            t = t->t_proc_next;
        }

        spin_unlock(&p->p_lock);
    }

    spin_unlock_irqrestore(&g_proc_list_lock, flags);
}
