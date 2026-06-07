#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <string.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static spinlock_t futex_lock = SPINLOCK_INITIALIZER;
static LIST_HEAD(g_futex_waiters);

extern uint64_t get_uptime_ns(void);

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, const void *timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)uaddr2;
    (void)val3;
    (void)timeout;
    int cmd = op & 127;

    if (cmd == FUTEX_WAIT) {
        if (!is_user_address_range(uaddr, sizeof(uint32_t))) {
            return -EFAULT;
        }

        uint64_t flags = spin_lock_irqsave(&futex_lock);

        uint32_t uval;
        if (copy_from_user(&uval, uaddr, sizeof(uint32_t)) < 0) {
            spin_unlock_irqrestore(&futex_lock, flags);
            return -EFAULT;
        }

        if (uval != val) {
            spin_unlock_irqrestore(&futex_lock, flags);
            return -EAGAIN;
        }

        struct thread *t = curthread;
        t->t_state = THREAD_WAITING;
        t->t_futex_addr = (uintptr_t)uaddr;
        t->t_lock_to_release = &futex_lock;

        list_add_tail(&t->t_wait_node, &g_futex_waiters);

        thread_yield();

        if (t->t_futex_addr != 0) {
            uint64_t f = spin_lock_irqsave(&futex_lock);
            if (t->t_wait_node.next && t->t_wait_node.prev) {
                list_del(&t->t_wait_node);
            }
            t->t_futex_addr = 0;
            spin_unlock_irqrestore(&futex_lock, f);
            return -EINTR;
        }

        return 0;
    } else if (cmd == FUTEX_WAKE) {
        if (!is_user_address_range(uaddr, sizeof(uint32_t))) {
            return -EFAULT;
        }

        uint64_t flags = spin_lock_irqsave(&futex_lock);

        int woken = 0;
        struct list_node *curr = g_futex_waiters.next;
        while (curr != &g_futex_waiters) {
            if (woken >= (int)val) {
                break;
            }
            struct thread *t = list_entry(curr, struct thread, t_wait_node);
            struct list_node *next_node = curr->next;

            if (t->t_futex_addr == (uintptr_t)uaddr) {
                list_del(curr);
                t->t_futex_addr = 0;
                t->t_state = THREAD_READY;
                sched_enqueue(t);
                woken++;
            }
            curr = next_node;
        }

        spin_unlock_irqrestore(&futex_lock, flags);
        return woken;
    }

    return -ENOSYS;
}

int64_t sys_clock_gettime(int clk_id, void *user_tp) {
    (void)clk_id;
    struct { int64_t tv_sec; int64_t tv_nsec; } tp;
    uint64_t ns = get_uptime_ns();
    tp.tv_sec = ns / 1000000000ULL;
    tp.tv_nsec = ns % 1000000000ULL;

    if (!is_user_address_range(user_tp, sizeof(tp))) return -EFAULT;
    if (copy_to_user(user_tp, &tp, sizeof(tp)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_clock_getres(int clk_id, void *user_tp) {
    (void)clk_id;
    struct { int64_t tv_sec; int64_t tv_nsec; } tp;
    tp.tv_sec = 0;
    tp.tv_nsec = 1;

    if (user_tp) {
        if (!is_user_address_range(user_tp, sizeof(tp))) return -EFAULT;
        if (copy_to_user(user_tp, &tp, sizeof(tp)) < 0) return -EFAULT;
    }
    return 0;
}

int64_t sys_getrlimit(int resource, void *user_rlim) {
    (void)resource;
    struct { uint64_t cur, max; } rl = { 8*1024*1024UL, (uint64_t)-1 };
    if (!is_user_address_range(user_rlim, sizeof(rl))) return -EFAULT;
    if (copy_to_user(user_rlim, &rl, sizeof(rl)) < 0) return -EFAULT;
    return 0;
}

#define ARCH_SET_FS 0x1002
extern void arch_cpu_set_fs_base(uint64_t addr);

int64_t sys_arch_prctl(int code, uintptr_t addr) {
    if (code == ARCH_SET_FS) {
        if (!is_user_address_range((void *)addr, sizeof(void *))) {
            return -EFAULT;
        }

        arch_cpu_set_fs_base(addr);

        curthread->t_fs_base = addr;

        return 0;
    }

    return -EINVAL;
}

int64_t sys_nanosleep(const struct timespec *user_req, struct timespec *user_rem) {
    if (!is_user_address_range(user_req, sizeof(struct timespec))) {
        return -EFAULT;
    }

    struct timespec req;
    if (copy_from_user(&req, user_req, sizeof(struct timespec)) < 0) {
        return -EFAULT;
    }

    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) {
        return -EINVAL;
    }

    if (req.tv_sec == 0 && req.tv_nsec == 0) {
        return 0;
    }

    uint64_t sec_limit = (uint64_t)-1 / 1000 - 1;
    uint64_t ms;
    if ((uint64_t)req.tv_sec > sec_limit) {
        ms = (uint64_t)-1 - 1000;
    } else {
        ms = (uint64_t)req.tv_sec * 1000 + (req.tv_nsec + 999999) / 1000000;
    }

    uint64_t start_ticks = arch_get_system_ticks();
    uint64_t max_ms = (uint64_t)-1 - start_ticks - 1000;
    if (ms > max_ms) {
        ms = max_ms;
    }

    uint64_t target_ticks = start_ticks + ms;

    thread_sleep(ms);

    uint64_t end_ticks = arch_get_system_ticks();
    if (end_ticks < target_ticks) {
        if (user_rem) {
            if (!is_user_address_range(user_rem, sizeof(struct timespec))) {
                return -EFAULT;
            }
            uint64_t remaining_ticks = target_ticks - end_ticks;
            struct timespec rem;
            rem.tv_sec = remaining_ticks / 1000;
            rem.tv_nsec = (remaining_ticks % 1000) * 1000000;
            if (copy_to_user(user_rem, &rem, sizeof(struct timespec)) < 0) {
                return -EFAULT;
            }
        }
        return -EINTR;
    }

    return 0;
}

int64_t sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if (buflen > 0 && !is_user_address_range(buf, buflen)) {
        return -EFAULT;
    }

    uint8_t chunk[256];
    size_t total = 0;
    while (total < buflen) {
        size_t to_copy = buflen - total;
        if (to_copy > sizeof(chunk)) {
            to_copy = sizeof(chunk);
        }
        for (size_t i = 0; i < to_copy; i++) {
            static uint64_t rand_val = 0;
            static int bytes_left = 0;
            if (bytes_left == 0) {
                static uint64_t xorshift_state = 0x123456789abcdef0ULL;
                uint32_t lo = 0, hi = 0;
                __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
                uint64_t x = xorshift_state ^ (((uint64_t)hi << 32) | lo);
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                xorshift_state = x;
                rand_val = x;
                bytes_left = 8;
            }
            chunk[i] = (uint8_t)(rand_val & 0xFF);
            rand_val >>= 8;
            bytes_left--;
        }
        if (copy_to_user((char *)buf + total, chunk, to_copy) < 0) {
            return -EFAULT;
        }
        total += to_copy;
    }
    return (int64_t)buflen;
}

