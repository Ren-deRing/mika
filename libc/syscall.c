#include <sys/syscall.h>
#include <stdarg.h>
#include <stdint.h>

int64_t syscall(uint64_t num, ...) {
    va_list ap;
    va_start(ap, num);
    uint64_t a1 = va_arg(ap, uint64_t);
    uint64_t a2 = va_arg(ap, uint64_t);
    uint64_t a3 = va_arg(ap, uint64_t);
    uint64_t a4 = va_arg(ap, uint64_t);
    uint64_t a5 = va_arg(ap, uint64_t);
    va_end(ap);

    int64_t ret;
    register uint64_t r10 asm("r10") = a4;
    register uint64_t r8  asm("r8")  = a5;

    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );

    return ret;
}

void* sbrk(intptr_t increment) {
    uintptr_t old_brk = syscall(SYS_brk, 0);
    if (increment == 0) {
        return (void*)old_brk;
    }

    uintptr_t new_brk = syscall(SYS_brk, old_brk + increment);
    if (new_brk == old_brk + increment) {
        return (void*)old_brk;
    } else {
        return (void*)-1;
    }
}
