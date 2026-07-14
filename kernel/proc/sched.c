#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>
#include <kernel/lock.h>
#include <kernel/init.h>
#include <kernel/intc.h>
#include <kernel/kmem.h>
#include <kernel/softirq.h>
#include <kernel/rcu.h>
#include <boot/bootinfo.h>
#include <string.h>
#include <kernel/printf.h>

#include <kernel/kstack.h>

extern void arch_context_switch(struct thread *prev, struct thread *next);

#define MLFQ_LEVELS 4

struct mlfq_queue {
    struct thread *head;
    struct thread *tail;
};

struct mlfq {
    spinlock_t lock;
    struct mlfq_queue queues[MLFQ_LEVELS];
    uint32_t thread_count;
};

static struct mlfq cpu_mlfqs[MAX_CPUS];
static const uint32_t mlfq_slices[MLFQ_LEVELS] = { 5, 10, 20, 40 };

static struct {
    spinlock_t lock;
    struct thread *head;
} sleep_queue;

void sched_enqueue(struct thread *t) {
    if (!t || t->t_tid == 0) return;

    if (t->t_cpu >= g_boot_info.smp.total_cores) {
        t->t_cpu = curcpu ? curcpu->id : 0;
    }

    uint32_t cpu_id = t->t_cpu;
    struct mlfq *q = &cpu_mlfqs[cpu_id];

    cpu_status_t flags = spin_lock_irqsave(&q->lock);
    t->t_state = THREAD_READY;
    t->t_sched_next = NULL;

    int prio = t->t_priority;
    if (prio < 0 || prio >= MLFQ_LEVELS) {
        prio = 0;
        t->t_priority = 0;
    }

    struct mlfq_queue *mq = &q->queues[prio];
    if (mq->tail) {
        mq->tail->t_sched_next = t;
    } else {
        mq->head = t;
    }
    mq->tail = t;
    q->thread_count++;

    spin_unlock_irqrestore(&q->lock, flags);

    struct cpu *this_cpu = curcpu;
    if (cpu_id != this_cpu->id) {
        struct cpu *target_cpu = &cpus[cpu_id];
        if (target_cpu->current_thread && target_cpu->current_thread->t_tid == 0) {
            arch_trigger_resched(cpu_id);
        }
    } else {
        if (this_cpu->current_thread && this_cpu->current_thread->t_tid == 0) {
            schedule();
        }
    }
}

struct thread* sched_dequeue(void) {
    uint32_t this_cpu_id = curcpu ? curcpu->id : 0;
    struct mlfq *my_q = &cpu_mlfqs[this_cpu_id];

    cpu_status_t flags = spin_lock_irqsave(&my_q->lock);

    struct thread *t = NULL;

    for (int p = 0; p < MLFQ_LEVELS; p++) {
        struct mlfq_queue *mq = &my_q->queues[p];
        t = mq->head;
        if (t) {
            mq->head = t->t_sched_next;
            if (!mq->head) mq->tail = NULL;
            t->t_sched_next = NULL;
            my_q->thread_count--;
            break;
        }
    }

    if (!t) {
        uint32_t total_cores = g_boot_info.smp.total_cores;
        for (uint32_t i = 0; i < total_cores; i++) {
            uint32_t target_cpu = (this_cpu_id + 1 + i) % total_cores;
            if (target_cpu == this_cpu_id) continue;

            struct mlfq *other_q = &cpu_mlfqs[target_cpu];
            if (other_q->thread_count == 0) continue;

            if (spin_trylock(&other_q->lock)) {
                for (int p = 0; p < MLFQ_LEVELS; p++) {
                    struct mlfq_queue *mq = &other_q->queues[p];
                    struct thread *stolen = mq->head;
                    if (stolen) {
                        mq->head = stolen->t_sched_next;
                        if (!mq->head) mq->tail = NULL;
                        stolen->t_sched_next = NULL;
                        other_q->thread_count--;

                        stolen->t_cpu = this_cpu_id;
                        t = stolen;
                        break;
                    }
                }
                spin_unlock(&other_q->lock);
                if (t) break; // 하하 니 일 이제 내꺼
            }
        }
    }

    if (t) {
        int prio = t->t_priority;
        if (prio < 0 || prio >= MLFQ_LEVELS) {
            prio = 0;
            t->t_priority = 0;
        }
        t->t_slice_left = mlfq_slices[prio];
        t->t_ticks = 0;
    }

    spin_unlock_irqrestore(&my_q->lock, flags);
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

__attribute__((unused)) static void dump_mlfq_queues(void) {
    uint32_t this_cpu_id = curcpu ? curcpu->id : 0;
    struct mlfq *q = &cpu_mlfqs[this_cpu_id];

    dprintf("[Sched Dump] CPU %d MLFQ Queues:\n", this_cpu_id);
    for (int p = 0; p < MLFQ_LEVELS; p++) {
        dprintf("  Prio %d: ", p);
        struct thread *curr = q->queues[p].head;
        if (!curr) {
            dprintf("<EMPTY>\n");
            continue;
        }
        while (curr) {
            dprintf("[TID:%d(%s) rem:%d]", curr->t_tid, get_state_name(curr->t_state), curr->t_slice_left);
            curr = curr->t_sched_next;
            if (curr) dprintf(" -> ");
        }
        dprintf("\n");
    }
}

void thread_post_switch_hook(void) {
    struct thread *last = curcpu->prev_thread;
    if (last) {
        if (last->t_lock_to_release) {
            spin_unlock(last->t_lock_to_release);
            last->t_lock_to_release = NULL;
        }

        if (last->t_state == THREAD_RUNNING && last->t_tid != 0) {
            sched_enqueue(last);
        } else if (last->t_state == THREAD_ZOMBIE) {
            if (last->t_kstack) {
                kstack_free(last->t_kstack);
            }
            arch_thread_destroy(last);
            kfree_aligned(last);
        }
        
        curcpu->prev_thread = NULL;
    }
}

void mi_switch(void) {
    cpu_status_t flags = arch_irq_save();

    struct thread *prev = curthread;

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

    curcpu->prev_thread = prev;

    rcu_report_qs();

    arch_context_switch(prev, next);

    thread_post_switch_hook();

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

    curthread->t_lock_to_release = &sleep_queue.lock;
    
    mi_switch();

    arch_irq_restore(flags);
}

static void sched_boost(void) {
    uint32_t total_cores = g_boot_info.smp.total_cores;
    for (uint32_t i = 0; i < total_cores; i++) {
        struct mlfq *q = &cpu_mlfqs[i];
        cpu_status_t flags = spin_lock_irqsave(&q->lock);

        for (int p = 1; p < MLFQ_LEVELS; p++) {
            struct mlfq_queue *mq = &q->queues[p];
            struct thread *t = mq->head;
            while (t) {
                struct thread *next_t = t->t_sched_next;
                t->t_priority = 0;
                t->t_slice_left = mlfq_slices[0];
                t->t_sched_next = NULL;

                struct mlfq_queue *mq0 = &q->queues[0];
                if (mq0->tail) {
                    mq0->tail->t_sched_next = t;
                } else {
                    mq0->head = t;
                }
                mq0->tail = t;

                t = next_t;
            }
            mq->head = NULL;
            mq->tail = NULL;
        }
        
        spin_unlock_irqrestore(&q->lock, flags);
    }
    
    cpu_status_t sleep_flags = spin_lock_irqsave(&sleep_queue.lock);
    struct thread *st = sleep_queue.head;
    while (st) {
        st->t_priority = 0;
        st = st->t_sched_next;
    }
    spin_unlock_irqrestore(&sleep_queue.lock, sleep_flags);
}

void sched_tick(void) {
    extern void posix_timers_tick(void);
    posix_timers_tick();

    uint64_t now = arch_get_system_ticks();
    
    uint64_t sleep_flags = spin_lock_irqsave(&sleep_queue.lock);
    while (sleep_queue.head && now >= sleep_queue.head->t_sleep_until) {
        struct thread *t = sleep_queue.head;
        sleep_queue.head = t->t_sched_next;
        sched_enqueue(t);
    }
    spin_unlock_irqrestore(&sleep_queue.lock, sleep_flags);

    if (now - curcpu->last_boost_tick >= 500) {
        curcpu->last_boost_tick = now;
        sched_boost();
    }

    struct thread *curr = curthread;
    if (curr) {
        if (curr->t_tid == 0) {
            schedule(); 
        } else {
            if (curr->t_slice_left > 0) {
                curr->t_slice_left--;
            }
            curr->t_ticks++;

            if (curr->t_slice_left == 0) {
                if (curr->t_priority < MLFQ_LEVELS - 1) {
                    curr->t_priority++;
                }
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
        sched_enqueue(t);
    }

    arch_irq_restore(flags);
}

static void sched_tick_softirq(struct trapframe *regs) {
    (void)regs;
    sched_tick();
}

void scheduler_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        spin_lock_init(&cpu_mlfqs[i].lock);
        for (int p = 0; p < MLFQ_LEVELS; p++) {
            cpu_mlfqs[i].queues[p].head = NULL;
            cpu_mlfqs[i].queues[p].tail = NULL;
        }
        cpu_mlfqs[i].thread_count = 0;
    }

    spin_lock_init(&sleep_queue.lock);

    arch_irq_save();

    open_softirq(TIMER_SOFTIRQ, sched_tick_softirq);
    open_softirq(RCU_SOFTIRQ, rcu_softirq_handler);

    arch_sched_init();

    arch_init_first_thread();
}

dev_initcall(scheduler_init, PRIO_LAST);