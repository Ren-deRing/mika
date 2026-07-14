/*
 * Mika Kernel Printf
 *
 * Copyright (c) 2017 Sean Barrett
 * Copyright (c) 2026 Ren-deRing (JONGHYUN WON)
 * 
 * SPDX-License-Identifier: 0BSD
 */

#include <kernel/cpu.h>
#include <kernel/printf.h>
#include <kernel/symbol.h>
#include <kernel/lock.h>

#define STB_SPRINTF_IMPLEMENTATION

#include <lib/stb_sprintf.h>

static kprint_callback_t active_kprint_sink;
static spinlock_t kprintf_lock = SPINLOCK_INITIALIZER;
static volatile int kprintf_depth = 0;
static volatile int kprintf_owner_cpu = -1;

void set_output_sink(kprint_callback_t new_sink) {
    active_kprint_sink = new_sink;
}

void vprintf(const char* format, va_list args) {
    if (!active_kprint_sink) return;

    char tmp[STB_SPRINTF_MIN];
    
    cpu_status_t flags;
    irq_save(flags);

    struct cpu *c = curcpu;
    int cpu_id = c ? (int)c->id : -1;
    bool needs_unlock = false;

    if (cpu_id != -1 && kprintf_owner_cpu == cpu_id) {
        kprintf_depth++;
    } else {
        spin_lock(&kprintf_lock);
        kprintf_owner_cpu = cpu_id;
        kprintf_depth = 1;
        needs_unlock = true;
    }

    stbsp_vsprintfcb(active_kprint_sink, NULL, tmp, format, args);

    if (needs_unlock) {
        kprintf_depth--;
        if (kprintf_depth == 0) {
            kprintf_owner_cpu = -1;
            spin_unlock(&kprintf_lock);
        }
    } else {
        kprintf_depth--;
    }

    irq_restore(flags);
}

void kputc(char c) {
    if (active_kprint_sink) {
        active_kprint_sink(&c, NULL, 1);
    }
}

void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void dprintf(const char* format, ...) { // TODO: Move to VFS
    va_list args;
    va_start(args, format);
    vprintf(format, args); // just doing this for now...
    va_end(args);
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = stbsp_vsnprintf(buf, size, fmt, args);
    va_end(args);
    return result;
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list args) {
    return stbsp_vsnprintf(buf, size, fmt, args);
}

extern int keyboard_queue_pop(void);
extern int copy_to_user(void *user_dest, const void *src, size_t n);
extern int copy_from_user(void *dest, const void *user_src, size_t n);

int64_t tty_read(void *user_buf, size_t count) {
    size_t copied = 0;
    char *ubuf = (char *)user_buf;
    static int kbd_is_escaped = 0;

    while (copied < count) {
        int sc = keyboard_queue_pop();
        if (sc < 0) break;

        uint8_t sc8 = (uint8_t)sc;

        if (sc8 == 0xE0) {
            kbd_is_escaped = 1;
            continue;
        }

        if (kbd_is_escaped) {
            kbd_is_escaped = 0;
            switch (sc8) {
                case 0x48: sc8 = 0x67; break; // Up Arrow Pressed
                case 0xC8: sc8 = 0xE7; break; // Up Arrow Released
                case 0x50: sc8 = 0x6c; break; // Down Arrow Pressed
                case 0xD0: sc8 = 0xEC; break; // Down Arrow Released
                case 0x4B: sc8 = 0x69; break; // Left Arrow Pressed
                case 0xCB: sc8 = 0xE9; break; // Left Arrow Released
                case 0x4D: sc8 = 0x6a; break; // Right Arrow Pressed
                case 0xCD: sc8 = 0xEA; break; // Right Arrow Released
                default: break;
            }
        }

        if (copy_to_user(ubuf + copied, &sc8, 1) < 0) {
            return (copied > 0) ? (int64_t)copied : -1;
        }
        copied++;
    }
    return (int64_t)copied;
}

int64_t tty_write(const void *user_buf, size_t count) {
    char kbuf[256];
    size_t total = 0;

    while (total < count) {
        size_t to_copy = count - total;
        if (to_copy > 255) to_copy = 255;

        if (copy_from_user(kbuf, (const char *)user_buf + total, to_copy) < 0) {
            return (total > 0) ? (int64_t)total : -1;
        }

        for (size_t i = 0; i < to_copy; i++) {
            kputc(kbuf[i]);
        }
        total += to_copy;
    }
    return (int64_t)count;
}

EXPORT_SYMBOL(printf);
EXPORT_SYMBOL(dprintf);
EXPORT_SYMBOL(snprintf);