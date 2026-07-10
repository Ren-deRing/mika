#include <kernel/rcu.h>
#include <kernel/cpu.h>
#include <kernel/lock.h>
#include <kernel/softirq.h>
#include <kernel/sched.h>
#include <kernel/printf.h>
#include <kernel/init.h>

#include <boot/bootinfo.h>

#include <string.h>

static struct {
    struct rcu_head *pending_head;
    struct rcu_head *pending_tail;
    int pending_count;

    struct rcu_head *done_head;
    struct rcu_head *done_tail;

    uint64_t gp_seq;
    uint64_t completed;
    int gp_active;
    int cpus_out;

    spinlock_t lock;
    int initialized;
} rcu_state;

void rcu_init(void) {
    memset(&rcu_state, 0, sizeof(rcu_state));
    spin_lock_init(&rcu_state.lock);
    rcu_state.initialized = 1;

    for (uint32_t i = 0; i < g_boot_info.smp.total_cores; i++)
        cpus[i].rcu_nesting = 0;
}

void rcu_read_lock(void) {
    if (!rcu_state.initialized) return;
    __atomic_fetch_add(&curcpu->rcu_nesting, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void rcu_read_unlock(void) {
    if (!rcu_state.initialized) return;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_fetch_sub(&curcpu->rcu_nesting, 1, __ATOMIC_RELAXED);
}

void call_rcu(struct rcu_head *head, rcu_callback_t func, void *arg) {
    if (!head) return;

    head->func = func;
    head->arg = arg;
    head->next = NULL;

    spin_lock(&rcu_state.lock);

    if (rcu_state.pending_tail)
        rcu_state.pending_tail->next = head;
    else
        rcu_state.pending_head = head;
    rcu_state.pending_tail = head;
    rcu_state.pending_count++;

    if (!rcu_state.gp_active) {
        rcu_state.gp_active = 1;
        rcu_state.gp_seq++;
        rcu_state.cpus_out = (int)g_boot_info.smp.total_cores;
        for (uint32_t i = 0; i < g_boot_info.smp.total_cores; i++)
            cpus[i].rcu_qs_pending = 1;
        raise_softirq(RCU_SOFTIRQ);
    }

    spin_unlock(&rcu_state.lock);
}

void rcu_report_qs(void) {
    if (!rcu_state.initialized) return;
    if (!rcu_state.gp_active) return;
    if (curcpu->rcu_nesting > 0) return;
    if (!curcpu->rcu_qs_pending) return;

    curcpu->rcu_qs_pending = 0;

    if (__atomic_fetch_sub(&rcu_state.cpus_out, 1, __ATOMIC_ACQ_REL) == 1) {
        spin_lock(&rcu_state.lock);

        struct rcu_head *p = rcu_state.pending_head;
        struct rcu_head *pt = rcu_state.pending_tail;
        uint64_t completed_gp = rcu_state.gp_seq;

        if (p) {
            rcu_state.pending_head = NULL;
            rcu_state.pending_tail = NULL;
            rcu_state.pending_count = 0;

            if (rcu_state.done_tail)
                rcu_state.done_tail->next = p;
            else
                rcu_state.done_head = p;
            rcu_state.done_tail = pt;
        }

        rcu_state.gp_active = 0;
        rcu_state.completed = completed_gp;

        spin_unlock(&rcu_state.lock);

        raise_softirq(RCU_SOFTIRQ);
    }
}

void synchronize_rcu(void) {
    if (!rcu_state.initialized) return;
    if (curcpu->rcu_nesting > 0)
        return;

    spin_lock(&rcu_state.lock);

    uint64_t wait_seq = rcu_state.completed + 1;

    if (!rcu_state.gp_active && rcu_state.pending_count > 0) {
        rcu_state.gp_active = 1;
        rcu_state.gp_seq++;
        rcu_state.cpus_out = (int)g_boot_info.smp.total_cores;
        for (uint32_t i = 0; i < g_boot_info.smp.total_cores; i++)
            cpus[i].rcu_qs_pending = 1;
        raise_softirq(RCU_SOFTIRQ);
    }

    spin_unlock(&rcu_state.lock);

    while (rcu_state.completed < wait_seq)
        thread_yield();
}

void rcu_softirq_handler(struct trapframe *regs) {
    (void)regs;

    if (!rcu_state.initialized) return;

    if (rcu_state.gp_active && curcpu->rcu_nesting == 0 && curcpu->rcu_qs_pending)
        rcu_report_qs();

    struct rcu_head *batch = NULL;

    spin_lock(&rcu_state.lock);
    if (rcu_state.done_head) {
        batch = rcu_state.done_head;
        rcu_state.done_head = NULL;
        rcu_state.done_tail = NULL;
    }
    spin_unlock(&rcu_state.lock);

    while (batch) {
        struct rcu_head *next = batch->next;
        batch->func(batch->arg);
        batch = next;
    }
}

dev_initcall(rcu_init, PRIO_FIRST);
