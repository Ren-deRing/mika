#pragma once

#include <kernel/proc.h>
#include <kernel/list.h>
#include <kernel/lock.h>
#include <kernel/cpu.h>
#include <kernel/sched.h>
#include <uapi/errno.h>

typedef struct {
    spinlock_t lock;
    struct list_node head;
} wait_queue_head_t;

#define DEFINE_WAIT_QUEUE(name) \
    wait_queue_head_t name = { .lock = SPINLOCK_INITIALIZER, .head = LIST_HEAD_INIT(name.head) }

static inline void init_waitqueue_head(wait_queue_head_t *wq) {
    spin_lock_init(&wq->lock);
    list_init(&wq->head);
}

static inline void add_wait_queue(wait_queue_head_t *wq, struct thread *t) {
    spin_lock(&wq->lock);
    if (t->t_wait_node.next == NULL)
        list_init(&t->t_wait_node);
    if (list_empty(&t->t_wait_node))
        list_add_tail(&t->t_wait_node, &wq->head);
    spin_unlock(&wq->lock);
}

static inline void remove_wait_queue(wait_queue_head_t *wq, struct thread *t) {
    spin_lock(&wq->lock);
    if (t->t_wait_node.next != NULL && !list_empty(&t->t_wait_node)) {
        list_del(&t->t_wait_node);
        list_init(&t->t_wait_node);
    }
    spin_unlock(&wq->lock);
}

#define __wait_event(wq, condition)                                     \
    do {                                                                 \
        for (;;) {                                                       \
            struct thread *__t = curthread;                              \
            __t->t_state = THREAD_WAITING;                               \
            add_wait_queue(wq, __t);                                     \
            __sync_synchronize();                                        \
            if (condition)                                               \
                break;                                                   \
            thread_yield();                                              \
        }                                                                \
        remove_wait_queue(wq, curthread);                                \
    } while (0)

#define wait_event_interruptible(wq, condition)                          \
    ({                                                                   \
        int __ret = 0;                                                   \
        for (;;) {                                                       \
            struct thread *__t = curthread;                              \
            __t->t_state = THREAD_WAITING;                               \
            add_wait_queue(wq, __t);                                     \
            __sync_synchronize();                                        \
            if (condition)                                               \
                break;                                                   \
                    if (__t->t_sig_pending) {                                    \
                __ret = -EINTR;                                              \
                break;                                                   \
            }                                                            \
            thread_yield();                                              \
        }                                                                \
        remove_wait_queue(wq, curthread);                                \
        __ret;                                                           \
    })

static inline void wake_up(wait_queue_head_t *wq) {
    spin_lock(&wq->lock);
    if (!list_empty(&wq->head)) {
        struct list_node *head_next = wq->head.next;
        if ((uintptr_t)head_next < 0x10) {
            spin_unlock(&wq->lock);
            return;
        }
        struct thread *t = list_first_entry(&wq->head, struct thread, t_wait_node);
        list_del(&t->t_wait_node);
        list_init(&t->t_wait_node);
        t->t_state = THREAD_READY;
        sched_enqueue(t);
    }
    spin_unlock(&wq->lock);
}

static inline void wake_up_all(wait_queue_head_t *wq) {
    spin_lock(&wq->lock);
    while (!list_empty(&wq->head)) {
        struct list_node *head_next = wq->head.next;
        if ((uintptr_t)head_next < 0x10) {
            spin_unlock(&wq->lock);
            return;
        }
        struct thread *t = list_first_entry(&wq->head, struct thread, t_wait_node);
        list_del(&t->t_wait_node);
        list_init(&t->t_wait_node);
        t->t_state = THREAD_READY;
        sched_enqueue(t);
    }
    spin_unlock(&wq->lock);
}
