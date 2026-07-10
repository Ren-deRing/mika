#pragma once

#include <stdint.h>
#include <stdbool.h>


struct trapframe;

typedef void (*rcu_callback_t)(void *arg);

struct rcu_head {
    struct rcu_head *next;
    rcu_callback_t func;
    void *arg;
};

void rcu_init(void);
void rcu_read_lock(void);
void rcu_read_unlock(void);
void call_rcu(struct rcu_head *head, rcu_callback_t func, void *arg);
void synchronize_rcu(void);
void rcu_report_qs(void);
void rcu_softirq_handler(struct trapframe *regs);
