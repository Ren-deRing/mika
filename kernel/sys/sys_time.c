#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <string.h>

extern uint64_t get_uptime_ns(void);

int64_t sys_time(int64_t *user_tloc) {
    uint64_t ns = get_uptime_ns();
    int64_t sec = ns / 1000000000ULL;
    if (user_tloc) {
        if (!is_user_address_range(user_tloc, sizeof(int64_t))) return -EFAULT;
        if (copy_to_user(user_tloc, &sec, sizeof(int64_t)) < 0) return -EFAULT;
    }
    return sec;
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

#ifdef __x86_64__
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
#endif

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
                uint64_t seed = arch_get_random_seed();
                uint64_t x = xorshift_state ^ seed;
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

#define MEMBARRIER_CMD_QUERY                          0
#define MEMBARRIER_CMD_GLOBAL                         1
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED               4
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED      8
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED             16
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED     32
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE    64
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE 128

int64_t sys_membarrier(int cmd, unsigned int flags, int cpu_id) {
    (void)flags;
    (void)cpu_id;

    switch (cmd) {
    case MEMBARRIER_CMD_QUERY:
        return MEMBARRIER_CMD_GLOBAL;
    case MEMBARRIER_CMD_GLOBAL:
    case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
        __sync_synchronize();
        return 0;
    default:
        return -EINVAL;
    }
}
