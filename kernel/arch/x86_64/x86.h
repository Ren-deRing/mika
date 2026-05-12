#pragma once

#include <kernel/cpu.h>

#include <uapi/types.h>

#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_EFER 0xC0000080
#define EFER_SCE (1ULL << 0)
#define MSR_LSTAR 0xC0000082
#define MSR_STAR  0xC0000081
#define MSR_SFMASK 0xC0000084

struct arch_proc {
    void *io_bitmap;
    uint32_t arch_flags;
};

extern uint8_t g_fpu_preset[4096];
extern size_t g_xsave_size;

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    asm volatile ("cpuid"
                  : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                  : "a"(leaf), "c"(subleaf));
}

static inline void xsetbv(uint32_t index, uint64_t value) {
    uint32_t eax = (uint32_t)value;
    uint32_t edx = (uint32_t)(value >> 32);
    asm volatile ("xsetbv" : : "a"(eax), "d"(edx), "c"(index));
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void wrmsr(uint32_t msr, uint64_t val);
uint64_t rdmsr(uint32_t msr);