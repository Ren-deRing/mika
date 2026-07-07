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

extern spinlock_t g_proc_list_lock;
extern struct list_node g_proc_list;

static spinlock_t futex_lock = SPINLOCK_INITIALIZER;
static LIST_HEAD(g_futex_waiters);

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, const void *timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)uaddr2;
    (void)val3;
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

        if (timeout != NULL) {
            struct timespec ts;
            if (copy_from_user(&ts, timeout, sizeof(struct timespec)) < 0) {
                spin_unlock_irqrestore(&futex_lock, flags);
                return -EFAULT;
            }
            uint64_t timeout_ms = (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
            spin_unlock_irqrestore(&futex_lock, flags);
            if (timeout_ms > 0) {
                thread_sleep(timeout_ms);
            }
            return -ETIMEDOUT;
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
