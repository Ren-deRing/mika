#include <kernel/kasan.h>
#include <kernel/printf.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <string.h>

extern void panic(const char* description, struct trapframe *regs);

static __no_sanitize_address void kasan_report(uintptr_t addr, size_t size, bool is_write, uint8_t shadow, uintptr_t ret_ip) {
    dprintf("[KASAN] %s of size %zu at 0x%lx, shadow=0x%02x, RIP=0x%lx\n",
            is_write ? "WRITE" : "READ", size, addr, shadow, ret_ip);
    arch_panic_halt();
}

static __no_sanitize_address void check_access(uintptr_t addr, size_t size, bool is_write) {
    uintptr_t shadow_base = (addr >> 3) + KASAN_SHADOW_OFFSET;
    for (size_t i = 0; i < size; ) {
        uint8_t sv = *(volatile uint8_t *)(shadow_base + (i >> 3));
        if (sv != 0) {
            size_t offset_in_granule = (addr + i) & 7;
            if (sv < 0x08) {
                if ((uint8_t)offset_in_granule >= sv) {
                    kasan_report(addr + i, size - i, is_write, sv, (uintptr_t)__builtin_return_address(0));
                }
                size_t skip = 8 - ((addr + i) & 7);
                i += (skip > (size - i)) ? (size - i) : skip;
                continue;
            } else {
                kasan_report(addr + i, size - i, is_write, sv, (uintptr_t)__builtin_return_address(0));
            }
        }
        i += 8 - ((addr + i) & 7);
    }
}

#define DEFINE_ASAN(size) \
    __no_sanitize_address void __asan_load##size(uintptr_t addr) { check_access(addr, size, false); } \
    __no_sanitize_address void __asan_store##size(uintptr_t addr) { check_access(addr, size, true); }

DEFINE_ASAN(1)
DEFINE_ASAN(2)
DEFINE_ASAN(4)
DEFINE_ASAN(8)
DEFINE_ASAN(16)

__no_sanitize_address void __asan_loadN(uintptr_t addr, uintptr_t size) { check_access(addr, size, false); }
__no_sanitize_address void __asan_storeN(uintptr_t addr, uintptr_t size) { check_access(addr, size, true); }

void __asan_handle_no_return(void) {}

void __asan_poison_stack_memory(uintptr_t addr, uintptr_t size) {
    for (uintptr_t i = 0; i < size; i++)
        *(volatile uint8_t *)(((addr + i) >> 3) + KASAN_SHADOW_OFFSET) = 0xF1;
}
void __asan_unpoison_stack_memory(uintptr_t addr, uintptr_t size) {
    for (uintptr_t i = 0; i < size; i++)
        *(volatile uint8_t *)(((addr + i) >> 3) + KASAN_SHADOW_OFFSET) = 0;
}

static __no_sanitize_address void set_shadow(const void *addr, size_t size, uint8_t val) {
    for (size_t i = 0; i < size; i += 8)
        *(volatile uint8_t *)mem_to_shadow((uint8_t *)addr + i) = val;
}

__no_sanitize_address void kasan_poison(const void *addr, size_t size, uint8_t val) {
    set_shadow(addr, size, val);
}

__no_sanitize_address void kasan_unpoison(const void *addr, size_t size) {
    set_shadow(addr, size, 0);
}

__no_sanitize_address void kasan_kmalloc(const void *ptr, size_t size, size_t class_size) {
    (void)size;
    if (!ptr) return;
    set_shadow(ptr, class_size, 0);
}

__no_sanitize_address void kasan_kfree(const void *ptr, size_t class_size) {
    if (!ptr) return;
    set_shadow(ptr, class_size, 0xFB);
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    for (size_t i = 0; i < class_size; i++)
        p[i] = 0xBE;
}

__no_sanitize_address void __asan_init(void) {}
void __asan_version_mismatch_check_v8(void) {}

__no_sanitize_address void *__asan_memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}
__no_sanitize_address void *__asan_memset(void *s, int c, size_t n) {
    return memset(s, c, n);
}

static __no_sanitize_address void kasan_halt(void) {
    dprintf("[KASAN] Halting CPU\n");
    __asm__ volatile("cli; 1: hlt; jmp 1b");
}

#define DEFINE_ASAN_REPORT(size) \
    __no_sanitize_address void __asan_report_load##size(uintptr_t addr) { \
        dprintf("[KASAN] Inline: load of size %d at 0x%lx, RIP=0x%lx\n", size, addr, (uintptr_t)__builtin_return_address(0)); \
        kasan_halt(); \
    } \
    __no_sanitize_address void __asan_report_store##size(uintptr_t addr) { \
        uint8_t shadow_val = *(volatile uint8_t *)((addr >> 3) + KASAN_SHADOW_OFFSET); \
        dprintf("[KASAN] Inline: store of size %d at 0x%lx, shadow=0x%02x, RIP=0x%lx\n", \
                size, addr, shadow_val, (uintptr_t)__builtin_return_address(0)); \
        kasan_halt(); \
    }

DEFINE_ASAN_REPORT(1)
DEFINE_ASAN_REPORT(2)
DEFINE_ASAN_REPORT(4)
DEFINE_ASAN_REPORT(8)
DEFINE_ASAN_REPORT(16)

__no_sanitize_address void __asan_report_load_n(uintptr_t addr, uintptr_t size) {
    dprintf("[KASAN] Inline: load_n of size %zu at 0x%lx, RIP=0x%lx\n", size, addr, (uintptr_t)__builtin_return_address(0));
    kasan_halt();
}
__no_sanitize_address void __asan_report_store_n(uintptr_t addr, uintptr_t size) {
    dprintf("[KASAN] Inline: store_n of size %zu at 0x%lx, RIP=0x%lx\n", size, addr, (uintptr_t)__builtin_return_address(0));
    kasan_halt();
}

__no_sanitize_address void kasan_init(void) {
    dprintf("[KASAN] Shadow range: 0x%lx - 0x%lx\n",
            (uintptr_t)KASAN_SHADOW_START, (uintptr_t)KASAN_SHADOW_END);
}

mem_initcall(kasan_init, PRIO_THIRD);
