#pragma once

#include <stdint.h>

struct ksym {
    const char *name;
    unsigned long addr;
};

#define EXPORT_SYMBOL(func) \
    __attribute__((section(".ksymtab"), used)) \
    static const struct ksym __ksym_##func = { #func, (unsigned long)func }

void kallsyms_init(void);
unsigned long kallsyms_lookup_name(const char *name);
const char *kallsyms_lookup(unsigned long addr);
uint32_t kallsyms_count(void);
const struct ksym *kallsyms_get_table(void);
