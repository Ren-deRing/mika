#include <kernel/softirq.h>
#include <kernel/cpu.h>
#include <kernel/printf.h>

static softirq_action_t softirq_vec[NR_SOFTIRQS];

void open_softirq(unsigned int nr, softirq_action_t action) {
    if (nr < NR_SOFTIRQS)
        softirq_vec[nr] = action;
}

void raise_softirq(unsigned int nr) {
    if (nr >= NR_SOFTIRQS) return;
    __atomic_fetch_or(&curcpu->softirq_pending, 1UL << nr, __ATOMIC_RELEASE);
}

void do_softirq(struct trapframe *regs) {
    if (curcpu->in_softirq)
        return;

    curcpu->in_softirq = true;

    bool limit_reached = false;
    int max_loop = 10;
    while (max_loop-- > 0) {
        uint64_t pending = __atomic_exchange_n(&curcpu->softirq_pending, 0,
                                                __ATOMIC_ACQ_REL);
        if (!pending)
            break;

        for (int i = 0; i < NR_SOFTIRQS && pending; i++) {
            if (pending & (1UL << i)) {
                if (softirq_vec[i])
                    softirq_vec[i](regs);
            }
        }
    }

    if (max_loop <= 0 && __atomic_load_n(&curcpu->softirq_pending, __ATOMIC_ACQUIRE)) {
        limit_reached = true;
    }

    if (limit_reached) {
        raise_softirq(TIMER_SOFTIRQ);
    }

    curcpu->in_softirq = false;
}
