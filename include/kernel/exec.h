#pragma once

#include <kernel/proc.h>
#include <stdint.h>

#define USER_STACK_TOP    0x7ffffffff000UL
#define USER_STACK_SIZE   (1024 * 1024)
#define USER_STACK_PAGES  (USER_STACK_SIZE / 4096)

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

int proc_exec(void *elf_data, char *const argv[], char *const envp[]);
int proc_execpath(struct proc *p, const char *path);

uintptr_t setup_user_stack(page_table_t *new_map, uintptr_t user_stack_top, 
                           char *const argv[], char *const envp[],
                           uintptr_t phdr_vaddr, uint64_t phnum,
                           uintptr_t interpreter_base, uintptr_t original_entry);

page_table_t* load_elf(void *elf_data, uintptr_t *out_entry, uintptr_t *out_brk, 
                      uintptr_t *out_phdr_vaddr, uint64_t *out_phnum,
                      uintptr_t *out_interpreter_base);