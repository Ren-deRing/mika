#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PRIO_FIRST      A
#define PRIO_SECOND     B
#define PRIO_THIRD      C
#define PRIO_FOURTH     D
#define PRIO_FIFTH      E
#define PRIO_LAST       Z

typedef void (*initcall_t)(void);

#define __INITCALL_SELECT(_1, _2, NAME, ...) NAME

#define __define_initcall(section_name, level, fn, sub) \
    static initcall_t __initcall_##level##sub##_##fn __attribute__((used, \
    section("." #section_name "." #level "." #sub), aligned(8))) = fn

#define __initcall_with_sub(section_name, level, fn, sub) __define_initcall(section_name, level, fn, sub)
#define __initcall_no_sub(section_name, level, fn) __define_initcall(section_name, level, fn, 5)


#define arch_initcall(...)   __INITCALL_SELECT(__VA_ARGS__, __initcall_with_sub, __initcall_no_sub)(early_initcall, 0, __VA_ARGS__)
#define mem_initcall(...)    __INITCALL_SELECT(__VA_ARGS__, __initcall_with_sub, __initcall_no_sub)(early_initcall, 1, __VA_ARGS__)
#define dev_initcall(...)    __INITCALL_SELECT(__VA_ARGS__, __initcall_with_sub, __initcall_no_sub)(early_initcall, 2, __VA_ARGS__)
#define subsys_initcall(...) __INITCALL_SELECT(__VA_ARGS__, __initcall_with_sub, __initcall_no_sub)(late_initcall, 3, __VA_ARGS__)
#define late_initcall(...)   __INITCALL_SELECT(__VA_ARGS__, __initcall_with_sub, __initcall_no_sub)(late_initcall, 4, __VA_ARGS__)

// AP
#define __ap_initcall_with_sub(level, fn, sub) \
    static initcall_t __ap_initcall_##level##sub##_##fn __attribute__((used, \
    section(".ap_initcall." #level "." #sub), aligned(8))) = fn

#define __ap_initcall_no_sub(level, fn) \
    static initcall_t __ap_initcall_##level##5_##fn __attribute__((used, \
    section(".ap_initcall." #level ".5"), aligned(8))) = fn

#define ap_arch_initcall(...)   __INITCALL_SELECT(__VA_ARGS__, __ap_initcall_with_sub, __ap_initcall_no_sub)(0, __VA_ARGS__)

extern initcall_t __early_initcall_start[];
extern initcall_t __early_initcall_end[];
extern initcall_t __late_initcall_start[];
extern initcall_t __late_initcall_end[];

extern initcall_t __ap_initcall_start[];
extern initcall_t __ap_initcall_end[];

void early_init(uint32_t hw_id);
void ap_early_init(uint32_t logic_id, uint32_t hw_id);

static inline void do_early_initcalls(void) {
    for (initcall_t* call = __early_initcall_start; call < __early_initcall_end; call++) {
        if (!call || !*call) continue;
        (*call)();
    }
}

static inline void do_late_initcalls(void) {
    for (initcall_t* call = __late_initcall_start; call < __late_initcall_end; call++) {
        if (!call || !*call) continue;
        (*call)();
    }
}

static inline void do_ap_initcalls(void) {
    for (initcall_t* call = __ap_initcall_start; call < __ap_initcall_end; call++) {
        if (!call || !*call) continue;
        (*call)();
    }
}