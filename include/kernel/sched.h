#pragma once

#include <stdint.h>
#include <stdbool.h>

struct thread;

void scheduler_init(void);
void schedule(void);
void thread_yield(void);
void thread_sleep(uint64_t ms);
void sched_tick(void);
struct thread* pick_next_thread(void);
void sched_enqueue(struct thread *t);
struct thread* sched_dequeue(void);

void mi_switch(void);
void thread_post_switch_hook(void);
void thread_signal_wakeup(struct thread *t);

extern volatile uint64_t g_ticks;