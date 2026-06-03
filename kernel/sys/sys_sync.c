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
