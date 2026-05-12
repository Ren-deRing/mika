#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>
#include <kernel/lock.h>
#include <kernel/init.h>
#include <kernel/intc.h>
#include <kernel/kmem.h>
#include <string.h>

extern void arch_context_switch(struct thread *prev, struct thread *next);

static struct {
    spinlock_t lock;
    struct thread *head;
    struct thread *tail;
} ready_queue;

static struct {
    spinlock_t lock;
    struct thread *head;
} sleep_queue;

void sched_enqueue(struct thread *t) {
    if (!t || t->t_tid == 0) return;

    spin_lock(&ready_queue.lock);
    t->t_state = THREAD_READY;
    t->t_sched_next = NULL;

    if (ready_queue.tail) {
        ready_queue.tail->t_sched_next = t;
    } else {
        ready_queue.head = t;
    }
    ready_queue.tail = t;
    spin_unlock(&ready_queue.lock);
}

struct thread* sched_dequeue(void) {
    spin_lock(&ready_queue.lock);
    struct thread *t = ready_queue.head;
    if (t) {
        ready_queue.head = t->t_sched_next;
        if (!ready_queue.head) ready_queue.tail = NULL;
        t->t_sched_next = NULL;
    }
    spin_unlock(&ready_queue.lock);
    return t;
}

void mi_switch(void) {
    cpu_status_t flags = arch_irq_save();

    struct thread *prev = curthread;
    struct thread *next = sched_dequeue();

    if (!next) {
        next = curcpu->idle;
    }

    if (prev->t_state == THREAD_RUNNING && prev->t_tid != 0) {
        sched_enqueue(prev);
    }

    if (prev == next) {
        arch_irq_restore(flags);
        return;
    }

    next->t_state = THREAD_RUNNING;
    curcpu->current_thread = next;

    curcpu->tss_rsp0 = (uintptr_t)next->t_kstack + KSTACK_SIZE;

    if (next->t_proc != prev->t_proc) {
        arch_switch_mm(prev->t_proc, next->t_proc);
    }

    arch_context_switch(prev, next);

    arch_irq_restore(flags);
}

void schedule(void) {
    if (curthread) {
        curthread->t_need_resched = true;
    }
}

void thread_yield(void) {
    mi_switch();
}

void thread_sleep(uint64_t ms) {
    cpu_status_t flags = arch_irq_save();
    
    curthread->t_sleep_until = arch_get_system_ticks() + ms;
    curthread->t_state = THREAD_SLEEP;

    spin_lock(&sleep_queue.lock);
    if (!sleep_queue.head || curthread->t_sleep_until < sleep_queue.head->t_sleep_until) {
        curthread->t_sched_next = sleep_queue.head;
        sleep_queue.head = curthread;
    } else {
        struct thread *curr = sleep_queue.head;
        while (curr->t_sched_next && curthread->t_sleep_until >= curr->t_sched_next->t_sleep_until) {
            curr = curr->t_sched_next;
        }
        curthread->t_sched_next = curr->t_sched_next;
        curr->t_sched_next = curthread;
    }
    spin_unlock(&sleep_queue.lock);
    
    arch_irq_restore(flags);

    mi_switch();
}

void sched_tick(void) {
    uint64_t now = arch_get_system_ticks();
    
    spin_lock(&sleep_queue.lock);
    while (sleep_queue.head && now >= sleep_queue.head->t_sleep_until) {
        struct thread *t = sleep_queue.head;
        sleep_queue.head = t->t_sched_next;
        sched_enqueue(t);
    }
    spin_unlock(&sleep_queue.lock);

    if (curthread && curthread->t_tid != 0) {
        curthread->t_ticks++;
        if (curthread->t_ticks >= 10) {
            curthread->t_ticks = 0;
            schedule();
        }
    }
}

void scheduler_init(void) {
    spin_lock_init(&ready_queue.lock);
    spin_lock_init(&sleep_queue.lock);

    arch_init_first_thread();
    // noreturn

    // g_intc->register_handler(0x40, arch_timer_handler, NULL);
    // g_intc->start_timer(1, 0x40);
}

dev_initcall(scheduler_init, PRIO_LAST);