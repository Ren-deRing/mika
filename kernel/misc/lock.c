#include <kernel/lock.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/printf.h>

void spin_lock_init(spinlock_t *lock) {
    lock->now_serving = 0;
    lock->next_ticket = 0;
    lock->holder_cpu = -1;
}

void spin_lock(spinlock_t *lock) {
    if (lock->holder_cpu == curcpu->id && lock->now_serving != lock->next_ticket) {
        dprintf("[Deadlock] Panic: CPU %d already holds this lock!\n", curcpu->id);
    }

    uint32_t ticket = __sync_fetch_and_add(&lock->next_ticket, 1);
    
    while (lock->now_serving != ticket) {
        arch_pause();
    }

    __sync_synchronize();
    lock->holder_cpu = curcpu->id;
}

void spin_unlock(spinlock_t *lock) {
    lock->holder_cpu = -1;
    __sync_synchronize();

    lock->now_serving++;
}

uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = arch_irq_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    arch_irq_restore(flags);
}

void mutex_init(mutex_t *m) {
    spin_lock_init(&m->wait_lock);
    m->locked = 0;
    m->owner = NULL;
    list_init(&m->wait_queue);
}

void mutex_lock(mutex_t *m) {
    uint64_t flags = spin_lock_irqsave(&m->wait_lock);

    while (m->locked) {
        struct thread *t = curthread;
        
        t->t_state = THREAD_WAITING;
        t->t_lock_to_release = &m->wait_lock;
        list_add_tail(&t->t_wait_node, &m->wait_queue);

        spin_unlock_irqrestore(&m->wait_lock, flags);
        thread_yield();

        flags = spin_lock_irqsave(&m->wait_lock);
    }

    m->locked = 1;
    m->owner = curthread;
    spin_unlock_irqrestore(&m->wait_lock, flags);
}

void mutex_unlock(mutex_t *m) {
    uint64_t flags = spin_lock_irqsave(&m->wait_lock);

    m->locked = 0;
    m->owner = NULL;

    if (!list_empty(&m->wait_queue)) {
        struct thread *waiter = list_first_entry(&m->wait_queue, struct thread, t_wait_node);
        list_del(&waiter->t_wait_node);
        
        waiter->t_state = THREAD_READY;
        sched_enqueue(waiter);
    }

    spin_unlock_irqrestore(&m->wait_lock, flags);
}