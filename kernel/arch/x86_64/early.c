#include <stdint.h>
#include <elf.h>
#include <stdbool.h>

#include "x86.h"

#include <boot/bootinfo.h>

#include <kernel/kmem.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/printf.h>

#define SERIAL_DEVICE 0x3F8

struct cpu cpus[MAX_CPUS];

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern bool g_use_xsave;

void fpu_init(void) {
    uintptr_t cr0, cr4;

    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // Emulation Off
    cr0 |= (1 << 1);  // Monitor Coprocessor On
    cr0 |= (1 << 5);  // Numeric Error On
    asm volatile ("mov %0, %%cr0" : : "r"(cr0));

    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);  // OSFXSR: FXSAVE/FXRSTOR On
    cr4 |= (1 << 10); // OSXMMEXCPT: Unmasked SSE Exception On
    cr4 |= (1 << 20); // SMEP: Supervisor Mode Execution Prevention
    cr4 |= (1 << 21); // SMAP: Supervisor Mode Access Prevention
    asm volatile ("mov %0, %%cr4" : : "r"(cr4));

    asm volatile ("fninit");

    uint32_t mxcsr = 0x1F80;
    asm volatile ("ldmxcsr %0" : : "m"(mxcsr));
}

void xsave_init(void) {
    uint32_t eax, ebx, ecx, edx;
    uintptr_t cr4;

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 26))) {
        g_use_xsave = false;
        g_xsave_size = 512;
        asm volatile ("fxsave (%0)" : : "r"(g_fpu_preset) : "memory");
        return; 
    }

    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 18); // OSXSAVE
    asm volatile ("mov %0, %%cr4" : : "r"(cr4));

    // Bit 0: x87, Bit 1: SSE, Bit 2: AVX
    uint64_t xcr0 = (1 << 0) | (1 << 1); 
    
    if (ecx & (1 << 28)) { // CPUID.1:ECX.AVX[bit 28]
        xcr0 |= (1 << 2);
    }

    xsetbv(0, xcr0);

    cpuid(0xD, 0, &eax, &ebx, &ecx, &edx);
    g_xsave_size = ebx;
    g_use_xsave = true;

    eax = 0xFFFFFFFF;
    edx = 0xFFFFFFFF;
    asm volatile ("xsave (%0)" 
              : 
              : "r"(g_fpu_preset), "a"(eax), "d"(edx) 
              : "memory");
}

extern void syscall_entry(void);

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    wrmsr(MSR_LSTAR, (uintptr_t)syscall_entry);

    uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x1B << 48);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_SFMASK, 0x200);
}

void cpu_init(uint32_t logic_id, uint32_t hw_id) {
    struct cpu *c = &cpus[logic_id];

    c->self = c;
    c->id = logic_id;
    c->hw_id = hw_id;

    for (int i = 0; i < KMEM_NUM_CLASSES; i++) {
        c->magazines[i] = NULL; 
    }
    
    wrmsr(MSR_GS_BASE, (uintptr_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, (uintptr_t)c);
}

static volatile int g_serial_lock = 0;

static char* log_write(const char* buffer, void* user, int size) {
    (void)user;

    uint64_t flags = arch_irq_save();
    while (__sync_lock_test_and_set(&g_serial_lock, 1)) {
        arch_pause();
    }

    for (int i = 0; i < size; ++i) {
        outb(SERIAL_DEVICE, buffer[i]);
    }

    __sync_lock_release(&g_serial_lock);
    arch_irq_restore(flags);

    return (char*)buffer;
}

static void log_init(void) {
	outb(SERIAL_DEVICE + 3, 0x03);
    set_output_sink(&log_write);
}

#include <kernel/version.h>

void early_init(uint32_t hw_id) {
    fpu_init();
    cpu_init(0, hw_id);
    log_init();
    xsave_init();
    syscall_init();

    dprintf("%s [%s]\n", __kernel_name, __kernel_version_codename);
}

void ap_early_init(uint32_t logic_id, uint32_t hw_id) {
    fpu_init();
    xsave_init();
    syscall_init();
    cpu_init(logic_id, hw_id);
}

/* Fixed: do_nothing(void)'s former kingdom... RIP 2026-2026 */