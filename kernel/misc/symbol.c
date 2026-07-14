#include <kernel/symbol.h>
#include <kernel/kmem.h>
#include <kernel/printf.h>
#include <kernel/init.h>

#define KSYM_HASH_SIZE 256

extern struct ksym __ksymtab_start[];
extern struct ksym __ksymtab_end[];

static struct ksym *ksym_table;
static uint32_t ksym_count;

static uint32_t ksym_hash(const char *name) {
    uint32_t h = 5381;
    while (*name) {
        h = ((h << 5) + h) + (unsigned char)*name;
        name++;
    }
    return h;
}

void kallsyms_init(void) {
    ksym_count = (uint32_t)(__ksymtab_end - __ksymtab_start);
    ksym_table = __ksymtab_start;
    dprintf("[kallsyms] %u symbols exported\n", ksym_count);
}

unsigned long kallsyms_lookup_name(const char *name) {
    if (!name || !ksym_table) return 0;
    uint32_t h = ksym_hash(name) % ksym_count;
    for (uint32_t i = 0; i < ksym_count; i++) {
        uint32_t idx = (h + i) % ksym_count;
        const struct ksym *s = &ksym_table[idx];
        if (s->name && __builtin_strcmp(s->name, name) == 0)
            return s->addr;
    }
    return 0;
}

const char *kallsyms_lookup(unsigned long addr) {
    if (!ksym_table) return NULL;
    for (uint32_t i = 0; i < ksym_count; i++) {
        if (ksym_table[i].addr == addr)
            return ksym_table[i].name;
    }
    return NULL;
}

uint32_t kallsyms_count(void) {
    return ksym_count;
}

const struct ksym *kallsyms_get_table(void) {
    return ksym_table;
}

late_initcall(kallsyms_init);
