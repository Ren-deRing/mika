#pragma once

#include <kernel/lock_types.h>
#include <kernel/cpu.h>
#include <stdbool.h>

#define SPINLOCK_INITIALIZER { .now_serving = 0, .next_ticket = 0, .holder_cpu = -1 }
#define MUTEX_INITIALIZER(name) { \
    .wait_lock = SPINLOCK_INITIALIZER, \
    .locked = 0, \
    .owner = NULL, \
    .wait_queue = LIST_HEAD_INIT((name).wait_queue); \
}

void spin_lock_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
bool spin_trylock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
uint64_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

#define RWLOCK_INITIALIZER { .val = 0 }

static inline void rwlock_init(rwlock_t *lock) {
    lock->val = 0;
}

static inline void read_lock(rwlock_t *lock) {
    uint32_t expected;
    do {
        expected = lock->val;
        if (expected & 0x80000000) {
            arch_pause();
            continue;
        }
    } while (!__sync_bool_compare_and_swap(&lock->val, expected, expected + 1));
    __sync_synchronize();
}

static inline void read_unlock(rwlock_t *lock) {
    __sync_synchronize();
    __sync_fetch_and_sub(&lock->val, 1);
}

static inline void write_lock(rwlock_t *lock) {
    uint32_t expected;
    do {
        expected = lock->val;
        if (expected != 0) {
            arch_pause();
            continue;
        }
    } while (!__sync_bool_compare_and_swap(&lock->val, 0, 0x80000000));
    __sync_synchronize();
}

static inline void write_unlock(rwlock_t *lock) {
    __sync_synchronize();
    lock->val = 0;
}

static inline uint64_t read_lock_irqsave(rwlock_t *lock) {
    uint64_t flags = arch_irq_save();
    read_lock(lock);
    return flags;
}

static inline void read_unlock_irqrestore(rwlock_t *lock, uint64_t flags) {
    read_unlock(lock);
    arch_irq_restore(flags);
}

static inline uint64_t write_lock_irqsave(rwlock_t *lock) {
    uint64_t flags = arch_irq_save();
    write_lock(lock);
    return flags;
}

static inline void write_unlock_irqrestore(rwlock_t *lock, uint64_t flags) {
    write_unlock(lock);
    arch_irq_restore(flags);
}