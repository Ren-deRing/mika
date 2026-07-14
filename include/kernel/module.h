#pragma once

#include <stdint.h>
#include <stddef.h>
#include <uapi/elf.h>

#define MODULE_NAME_LEN     64
#define MODULE_REGION_BASE  0xFFFFFFFFA0000000ULL
#define MODULE_REGION_SIZE  0x400000000ULL

#define MODULE_STATE_LOADED   0
#define MODULE_STATE_COMING   1
#define MODULE_STATE_GOING    2

struct module {
    char name[MODULE_NAME_LEN];
    void *base;
    size_t size;
    int (*init)(void);
    void (*exit)(void);
    int refcount;
    int state;
    struct module *next;
};

struct module_secmap {
    uint16_t elf_shndx;
    uintptr_t loaded_addr;
};

void module_alloc_init(void);
void *module_alloc(size_t size);
void module_free(void *ptr, size_t size);
void module_set_rx(void *base, size_t size);
void module_set_ro(void *base, size_t size);

struct module *module_load_from_fd(int fd);
int module_apply_rela(void *base, const Elf64_Rela *rela, uint32_t count,
                      Elf64_Sym *symtab, const char *strtab, size_t strtab_size,
                      const struct module_secmap *secmap, uint32_t secmap_count);
int module_load(struct module *mod);
int module_unload(const char *name);
struct module *module_find(const char *name);

void module_list_lock(void);
void module_list_unlock(void);
