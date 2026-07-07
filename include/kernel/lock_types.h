#pragma once

#include <stdint.h>
#include <kernel/list.h>

typedef struct {
    volatile uint32_t now_serving;
    volatile uint32_t next_ticket;
    volatile int holder_cpu;
} spinlock_t;

struct thread;

typedef struct {
    spinlock_t wait_lock;
    int locked;
    struct thread *owner;
    struct list_node wait_queue;
} mutex_t;

typedef struct {
    volatile uint32_t val;
} rwlock_t;

typedef struct {
    spinlock_t wait_lock;
    int count;           /* >0: readers, 0: unlocked, -1: write-locked */
    struct thread *owner; /* write owner (for debug/recursion check) */
    struct list_node wait_queue;
} rw_semaphore_t;