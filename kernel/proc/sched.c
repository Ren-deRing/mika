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

static const char* get_state_name(thread_state_t state) {
    switch (state) {
        case THREAD_RUNNING: return "RUNNING";
        case THREAD_READY:   return "READY";
        case THREAD_SLEEP:   return "SLEEP";
        case THREAD_ZOMBIE:  return "ZOMBIE";
        default:             return "UNKNOWN";
    }
}

#include <kernel/printf.h>

__attribute__((unused)) static void dump_ready_queue(void) {
    struct thread *curr = ready_queue.head;
    
    dprintf("[Sched Dump] Ready Queue: ");
    if (!curr) {
        dprintf("<EMPTY>\n");
        return;
    }

    while (curr) {
        dprintf("[TID:%d(%s)]", curr->t_tid, get_state_name(curr->t_state));
        
        curr = curr->t_sched_next;
        if (curr) {
            dprintf(" -> ");
        }
    }
    dprintf("\n");
}

void mi_switch(void) {
    cpu_status_t flags = arch_irq_save();

    struct thread *prev = curthread;

    if (prev->t_state == THREAD_RUNNING && prev->t_tid != 0) {
        sched_enqueue(prev);
    }

    // spin_lock(&ready_queue.lock);
    // dump_ready_queue(); 
    // spin_unlock(&ready_queue.lock);

    struct thread *next = sched_dequeue();

    if (!next) {
        next = curcpu->idle;
    }

    if (prev == next && prev->t_state != THREAD_ZOMBIE) {
        prev->t_state = THREAD_RUNNING;
        if (prev->t_lock_to_release) {
            spin_unlock(prev->t_lock_to_release);
            prev->t_lock_to_release = NULL;
        }
        arch_irq_restore(flags);
        return;
    }

    if (prev == next && prev->t_state == THREAD_ZOMBIE) {
        next = curcpu->idle; 
    }

    next->t_state = THREAD_RUNNING;
    curcpu->current_thread = next;

    curcpu->tss_rsp0 = (uintptr_t)next->t_kstack + KSTACK_SIZE;
    arch_set_kernel_stack(curcpu->tss_rsp0);

    if (next->t_proc != prev->t_proc) {
        arch_switch_mm(prev->t_proc, next->t_proc);
    }

    if (prev->t_lock_to_release) {
        spin_unlock(prev->t_lock_to_release);
        prev->t_lock_to_release = NULL;
    }

    // dprintf("[SWITCH] Switching from TID %d (%s) to TID %d (%s), prev_fs: %p, next_fs: %p\n", 
    //         prev->t_tid, get_state_name(prev->t_state), next->t_tid, get_state_name(next->t_state),
    //         (void*)prev->t_fs_base, (void*)next->t_fs_base);

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

    struct thread *curr = curthread;
    if (curr) {
        if (curr->t_tid == 0) {
            schedule(); 
        } else {
            curr->t_ticks++;
            if (curr->t_ticks >= 10) {
                curr->t_ticks = 0;
                schedule();
            }
        }
    }
}

void thread_signal_wakeup(struct thread *t) {
    if (!t) return;

    cpu_status_t flags = arch_irq_save();

    if (t->t_state == THREAD_SLEEP) {
        spin_lock(&sleep_queue.lock);
        if (sleep_queue.head == t) {
            sleep_queue.head = t->t_sched_next;
            t->t_sched_next = NULL;
            spin_unlock(&sleep_queue.lock);
            sched_enqueue(t);
        } else {
            struct thread *curr = sleep_queue.head;
            while (curr && curr->t_sched_next != t) {
                curr = curr->t_sched_next;
            }
            if (curr) {
                curr->t_sched_next = t->t_sched_next;
                t->t_sched_next = NULL;
                spin_unlock(&sleep_queue.lock);
                sched_enqueue(t);
            } else {
                spin_unlock(&sleep_queue.lock);
            }
        }
    } else if (t->t_state == THREAD_WAITING) {
        if (t->t_wait_node.next && t->t_wait_node.prev) {
            list_del(&t->t_wait_node);
        }
        sched_enqueue(t);
    }

    arch_irq_restore(flags);
}

void scheduler_init(void) {
    spin_lock_init(&ready_queue.lock);
    spin_lock_init(&sleep_queue.lock);

    arch_irq_save();

    g_intc->register_handler(0x40, arch_timer_handler, NULL);
    g_intc->start_timer(1, 0x40);

    arch_init_first_thread();
    // noreturn
}

dev_initcall(scheduler_init, PRIO_LAST);