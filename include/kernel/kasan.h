#pragma once
#include <stdint.h>
#include <stddef.h>

#if __has_attribute(no_sanitize_address)
#define __no_sanitize_address __attribute__((no_sanitize("address")))
#else
#define __no_sanitize_address
#endif

#define KASAN_SHADOW_OFFSET 0xdffffc0000000000ULL
#define KASAN_SHADOW_SCALE  3ULL

#define KASAN_SHADOW_START  (KASAN_SHADOW_OFFSET + (0xFFFF800000000000ULL >> KASAN_SHADOW_SCALE))
#define KASAN_SHADOW_END    (KASAN_SHADOW_OFFSET + (0xFFFFFFFFFFFFFFFFULL >> KASAN_SHADOW_SCALE))

static inline uint8_t *mem_to_shadow(const void *addr) {
    return (uint8_t *)(((uintptr_t)addr >> KASAN_SHADOW_SCALE) + KASAN_SHADOW_OFFSET);
}

void kasan_init(void);
void kasan_kmalloc(const void *ptr, size_t size, size_t class_size);
void kasan_kfree(const void *ptr, size_t class_size);
void __no_sanitize_address kasan_poison(const void *addr, size_t size, uint8_t val);
void __no_sanitize_address kasan_unpoison(const void *addr, size_t size);
