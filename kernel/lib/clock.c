#include <kernel/cpu.h>
#include <kernel/clock.h>
#include <kernel/printf.h>

static struct clock_source *current_clock = 0;
static uint64_t boot_time_ns = 0;

void clock_register_source(struct clock_source *source) {
    if (!current_clock || source->rating > current_clock->rating) {
        current_clock = source;
    }
}

uint64_t get_uptime_ns(void) {
    if (!current_clock) return 0;
    
    return current_clock->ticks_to_ns(current_clock->read());
}

void udelay(uint64_t us) {
    if (!current_clock) return;

    uint64_t start = get_uptime_ns();
    uint64_t target = us * 1000; // us -> ns

    while ((get_uptime_ns() - start) < target) {
        arch_pause();
    }
}