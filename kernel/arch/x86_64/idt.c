#include <kernel/init.h>
#include <kernel/cpu.h>
#include <kernel/printf.h>
#include <kernel/proc.h>
#include <kernel/mmu.h>

static inline uintptr_t read_cr2(void) {
    uintptr_t val;
    asm volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
} // TODO

struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t int_no;
    uint64_t err_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

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

void panic(const char* description, struct registers *regs) {
    dprintf("\n[KERNEL PANIC] %s (Vector: %d)\n", description, regs->int_no);
    dprintf("Error Code: %08x\n", regs->err_code);
    dprintf("RIP: %016llx  RSP: %016llx\n", regs->rip, regs->rsp);
    dprintf("CS: %02x  SS: %02x  RFLAGS: %08x\n", regs->cs, regs->ss, regs->rflags);

    asm volatile ("outb %b0, %w1" : : "a"(1), "Nd"(0xf4));
    for (;;) arch_halt();
}

void page_fault_handler(struct registers *regs, void *data) {
    uintptr_t fault_addr = read_cr2();
    struct thread *thr = curthread;

    if (thr && thr->t_kstack) {
        uintptr_t stack_start = (uintptr_t)thr->t_kstack;
        uintptr_t guard_page_start = stack_start - PAGE_SIZE;

        if (fault_addr >= guard_page_start && fault_addr < stack_start) {
            panic("Kernel Stack Overflow", regs);
            return;
        }
    }
    
    panic(exceptions[14], regs);
}

void idt_install(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t ist_index = (i == 8) ? 1 : 0;
        if (i == 8 || i == 14) ist_index = 1;
        idt_set_descriptor(i, (void*)isr_stub_table[i], 0x8E, ist_index);
    }

    register_handler(14, page_fault_handler, NULL);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uintptr_t)&idt;
    
    asm volatile ("lidt %0" : : "m"(idtr));
}

void ap_idt_install(void) {
	asm volatile ("lidt %0" : : "m"(idtr));
}

static void isr_handler_inner(struct registers *regs) {
    uint8_t vector = (uint8_t)regs->int_no;
    struct isr_slot *slot = &handlers[vector];

    if (slot->func) {
        slot->func(regs, slot->data);
    } else {
        if (vector < 32) {
            panic(exceptions[regs->int_no], regs);
        } else if (vector >= 32 && vector < 48) {
            // ack_irq(vector - 32); 
        }
    }
}

void isr_handler(struct registers *regs) {
    isr_handler_inner(regs);
}

arch_initcall(idt_install, PRIO_SECOND);

ap_arch_initcall(ap_idt_install, PRIO_SECOND);