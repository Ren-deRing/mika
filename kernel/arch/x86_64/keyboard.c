#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/intc.h>
#include <kernel/lock.h>
#include <kernel/printf.h>

#define KBD_QUEUE_SIZE 128
static uint8_t kbd_queue[KBD_QUEUE_SIZE];
static int kbd_head = 0;
static int kbd_tail = 0;

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern struct interrupt_controller* g_intc;

void keyboard_queue_push(uint8_t scancode) {
    cpu_status_t flags;
    irq_save(flags);
    int next = (kbd_tail + 1) % KBD_QUEUE_SIZE;
    if (next != kbd_head) {
        kbd_queue[kbd_tail] = scancode;
        kbd_tail = next;
    }
    irq_restore(flags);
}

int keyboard_queue_pop(void) {
    cpu_status_t flags;
    irq_save(flags);
    if (kbd_head == kbd_tail) {
        irq_restore(flags);
        return -1; // empty
    }
    uint8_t sc = kbd_queue[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_QUEUE_SIZE;
    irq_restore(flags);
    return sc;
}

static void kbd_wait_write(void) {
    uint32_t timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout) {
        asm volatile("pause");
    }
}

static void kbd_wait_read(void) {
    uint32_t timeout = 100000;
    while (!(inb(0x64) & 0x01) && --timeout) {
        asm volatile("pause");
    }
}

static void keyboard_handler(struct trapframe *regs, void *data) {
    (void)regs; (void)data;
    uint8_t sc = inb(0x60);
    keyboard_queue_push(sc);
    g_intc->eoi();
}

void keyboard_init(void) {
    kbd_wait_write();
    outb(0x64, 0xAE);

    kbd_wait_write();
    outb(0x64, 0x20);
    kbd_wait_read();
    uint8_t config = inb(0x60);
    dprintf("[KBD] Current 8042 config: 0x%02X\n", config);

    config |= 0x01;
    config &= ~0x10;
    
    kbd_wait_write();
    outb(0x64, 0x60);
    kbd_wait_write();
    outb(0x60, config);

    uint32_t flush_count = 0;
    while (inb(0x64) & 0x01) {
        uint8_t stale = inb(0x60);
        (void)stale;
        flush_count++;
    }

    kbd_wait_write();
    outb(0x60, 0xF4);
    
    uint32_t ack_timeout = 100000;
    while (!(inb(0x64) & 0x01) && --ack_timeout) {
        asm volatile("pause");
    }
    if (inb(0x64) & 0x01) {
        uint8_t response = inb(0x60);
    }
    if (!g_intc) {
        return;
    }
    g_intc->register_handler(0x21, keyboard_handler, NULL);
    g_intc->map_irq(1, 0, 0x21);
    g_intc->unmask(1);
}

dev_initcall(keyboard_init, PRIO_THIRD);
