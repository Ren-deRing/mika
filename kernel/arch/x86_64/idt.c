#include <kernel/init.h>
#include <kernel/cpu.h>
#include <kernel/printf.h>
#include <kernel/proc.h>
#include <kernel/mmu.h>
#include <uapi/signal.h>
#include <kernel/sched.h>
#include "x86.h"

static inline uintptr_t read_cr2(void) {
    uintptr_t val;
    asm volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
} // TODO



struct isr_slot {
    handler_t func;
    void *data;
};

typedef struct {
    uint16_t isr_low;      // ISR 주소 하위 16비트
    uint16_t kernel_cs;    // CS
    uint8_t  ist;          // IST
    uint8_t  attributes;   // 권한 & 타입
    uint16_t isr_mid;      // ISR 주소 중간 16비트
    uint32_t isr_high;     // ISR 주소 상위 32비트
    uint32_t reserved;     // 예약됨
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

static volatile idt_entry_t idt[256];
static volatile idtr_t idtr;
static struct isr_slot handlers[256];

extern volatile void* isr_stub_table[];

void register_handler(uint8_t vector, handler_t handler, void *data) {
    handlers[vector].func = handler;
    handlers[vector].data = data;
}

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags, uint8_t ist) {
    uintptr_t addr = (uintptr_t)isr;
    idt[vector].isr_low    = addr & 0xFFFF;
    idt[vector].kernel_cs  = 0x08;       // GDT CS
    idt[vector].ist        = ist & 0x07; // 하위 3비트
    idt[vector].attributes = flags;
    idt[vector].isr_mid    = (addr >> 16) & 0xFFFF;
    idt[vector].isr_high   = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved   = 0;
}

const char* exceptions[32] = {
    "Divide-by-zero Error",
    "Debug",                          // 1
    "Non-maskable Interrupt",         // 2
    "Breakpoint",                     // 3
    "Overflow",                       // 4
    "Bound Range Exceeded",           // 5
    "Invalid Opcode",                 // 6
    "Device Not Available",           // 7
    "Double Fault",                   // 8
    "Coprocessor Segment Overrun",    // 9 (Reserved in newer CPUs)
    "Invalid TSS",                    // 10
    "Segment Not Present",            // 11
    "Stack-Segment Fault",            // 12
    "General Protection Fault",       // 13
    "Page Fault",                     // 14
    "Reserved",                       // 15
    "x87 Floating-Point Exception",   // 16
    "Alignment Check",                // 17
    "Machine Check",                  // 18
    "SIMD Floating-Point Exception",  // 19
    "Virtualization Exception",       // 20
    "Control Protection Exception",   // 21
    "Reserved",                       // 22
    "Reserved",                       // 23
    "Reserved",                       // 24
    "Reserved",                       // 25
    "Reserved",                       // 26
    "Reserved",                       // 27
    "Hypervisor Injection Exception", // 28
    "VMM Communication Exception",    // 29
    "Security Exception",             // 30
    "Reserved"                        // 31
};

void panic(const char* description, struct trapframe *regs) {
    uintptr_t fault_addr = read_cr2();

    dprintf("\n[KERNEL PANIC] %s (Vector: %d)\n", description, regs->int_no);
    dprintf("GS_BASE: %016llx  KERNEL_GS_BASE: %016llx\n", rdmsr(MSR_GS_BASE), rdmsr(MSR_KERNEL_GS_BASE));
    dprintf("Error Code: %08x\n", regs->err_code);
    dprintf("RIP: %016llx  RSP: %016llx\n", regs->rip, regs->rsp);
    dprintf("CS: %02x  SS: %02x  RFLAGS: %08x\n", regs->cs, regs->ss, regs->rflags);
    if (regs->int_no == 14) {
        dprintf("CR2: %016lx\n", fault_addr);
        
        uintptr_t write_target = regs->rsp - 8;
        dprintf("Expected write target: %016lx\n", write_target);
        
        if (curthread && curthread->t_proc && curthread->t_proc->p_vm_map) {
            uintptr_t paddr = mmu_translate(curthread->t_proc->p_vm_map, write_target);
            dprintf("mmu_translate(write_target): %016lx\n", paddr);
            paddr = mmu_translate(curthread->t_proc->p_vm_map, regs->rsp);
            dprintf("mmu_translate(rsp): %016lx\n", paddr);
        }
    }

    asm volatile ("outb %b0, %w1" : : "a"(1), "Nd"(0xf4));
    for (;;) arch_halt();
}

void idt_install(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t ist_index = 0;
        if (i == 8) ist_index = 1;
        idt_set_descriptor(i, (void*)isr_stub_table[i], 0x8E, ist_index);
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uintptr_t)&idt;
    
    asm volatile ("lidt %0" : : "m"(idtr));
}

void ap_idt_install(void) {
	asm volatile ("lidt %0" : : "m"(idtr));
}

static void isr_handler_inner(struct trapframe *regs) {
    uint8_t vector = (uint8_t)regs->int_no;
    struct isr_slot *slot = &handlers[vector];

    if (slot->func) {
        slot->func(regs, slot->data);
    } else {
        if (vector < 32) {
            if ((regs->cs & 3) == 3) {
                int sig = SIGSEGV;
                if (vector == 0 || vector == 16 || vector == 19) {
                    sig = SIGFPE;
                } else if (vector == 6) {
                    sig = SIGILL;
                } else if (vector == 1 || vector == 3) {
                    sig = SIGTRAP;
                } else if (vector == 11 || vector == 17) {
                    sig = SIGBUS;
                }
                
                if (curproc && curthread) {
                    uint64_t lock_flags = spin_lock_irqsave(&curproc->p_lock);
                    curthread->t_sig_pending |= (1ULL << (sig - 1));
                    spin_unlock_irqrestore(&curproc->p_lock, lock_flags);
                    thread_signal_wakeup(curthread);
                    return;
                }
            }
            panic(exceptions[regs->int_no], regs);
        } else if (vector >= 32 && vector < 48) {
            // ack_irq(vector - 32); 
        }
    }
}

void isr_handler(struct trapframe *regs) {
    isr_handler_inner(regs);
    if (curthread && (regs->cs & 3) == 3) {
        curthread->t_trapframe = regs;
        check_signals(regs);
    }
}

arch_initcall(idt_install, PRIO_SECOND);

ap_arch_initcall(ap_idt_install, PRIO_SECOND);