#pragma once

#include <stdint.h>

static inline int atomic_inc_not_zero(uint32_t *ptr) {
    uint32_t old;
    do {
        old = *ptr;
        if (old == 0) return 0;
    } while (!__sync_bool_compare_and_swap(ptr, old, old + 1));
    return 1;
}
