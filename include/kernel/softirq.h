#pragma once

#include <kernel/cpu.h>

#define TIMER_SOFTIRQ 0
#define NR_SOFTIRQS   8

typedef void (*softirq_action_t)(struct trapframe *regs);

void open_softirq(unsigned int nr, softirq_action_t action);
void raise_softirq(unsigned int nr);
void do_softirq(struct trapframe *regs);
