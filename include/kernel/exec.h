#pragma once

#include <kernel/proc.h>
#include <stdint.h>

#define USER_STACK_TOP    0x7FFFFFFEFFFF
#define USER_STACK_SIZE   (1024 * 1024)
#define USER_STACK_PAGES  (USER_STACK_SIZE / 4096)


int proc_exec(struct proc *p, void *elf_data);
int proc_execpath(struct proc *p, const char *path);

int setup_user_stack(page_table_t *map, uintptr_t stack_top, size_t size);

page_table_t* load_elf(void *elf_data, uintptr_t *out_entry, uintptr_t *out_brk);