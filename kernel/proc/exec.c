#include <kernel/exec.h>
#include <kernel/mmu.h>
#include <kernel/kmem.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>
#include <kernel/sched.h>

#include <uapi/elf.h>
#include <uapi/errno.h>

#include <string.h>

extern void arch_enter_user_mode(uintptr_t entry, uintptr_t user_rsp);

#include <kernel/printf.h>

void arch_user_trampoline(void *arg) {
    struct proc *p = (struct proc *)arg;
    struct thread *self = curthread;

    arch_set_kernel_stack((uintptr_t)self->t_kstack + KSTACK_SIZE);
    
    arch_switch_mm(NULL, p); 
    
    arch_enter_user_mode(p->p_entry, p->p_stack_top);
}

int proc_exec(void *elf_data, char *const argv[], char *const envp[]) {
    uintptr_t entry_point = 0;
    uintptr_t brk = 0;
    uintptr_t phdr_vaddr = 0;
    uint64_t phnum = 0;

    page_table_t *new_map = load_elf(elf_data, &entry_point, &brk, &phdr_vaddr, &phnum);
    if (!new_map) {
        return -ENOEXEC;
    }

    struct proc *p = proc_create(next_pid++);
    if (!p) {
        mmu_destroy_map(new_map);
        return -ENOMEM;
    }

    uintptr_t stack_top = USER_STACK_TOP;
    uintptr_t stack_bottom = stack_top - USER_STACK_SIZE;

    for (uintptr_t curr = stack_bottom; curr < stack_top; curr += PAGE_SIZE) {
        page_t *pg = page_alloc(0);
        if (!pg) {
            return -ENOMEM;
        }
        uintptr_t paddr = page_to_phys(pg);
        memset(p2v(paddr), 0, PAGE_SIZE);
        
        mmu_map(new_map, curr, paddr, MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_EXEC);
    }

    uintptr_t final_rsp = setup_user_stack(new_map, USER_STACK_TOP, argv, envp, phdr_vaddr, phnum);

    p->p_vm_map = new_map;
    p->p_entry = entry_point;
    p->p_stack_top = final_rsp;
    p->p_brk = ALIGN_UP(brk, PAGE_SIZE);

    struct thread *new_t = thread_create(p, next_tid++, arch_user_trampoline, (void *)p);
    if (!new_t) {
        return -ENOMEM;
    }

    new_t->t_flags |= THREAD_FLAG_USER;
    new_t->t_user_stack_top = final_rsp;
    sched_enqueue(new_t);

    return 0;
}

#define MAX_ARG_PAGES 4
#define TMP_STACK_SIZE (MAX_ARG_PAGES * PAGE_SIZE)

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHNUM   4
#define AT_PHENT   5
#define AT_PAGESZ  6
#define AT_RANDOM 25

uintptr_t setup_user_stack(page_table_t *new_map, uintptr_t user_stack_top, 
                           char *const argv[], char *const envp[],
                           uintptr_t phdr_vaddr, uint64_t phnum) {
    uint8_t *kbuf = kmalloc(TMP_STACK_SIZE);
    memset(kbuf, 0, TMP_STACK_SIZE);

    int argc = 0; if (argv) { while (argv[argc]) argc++; }
    int envc = 0; if (envp) { while (envp[envc]) envc++; }

    size_t strings_size = 0;
    for (int i = 0; i < argc; i++) { strings_size += strlen(argv[i]) + 1; }
    for (int i = 0; i < envc; i++) { strings_size += strlen(envp[i]) + 1; }

    size_t table_elements = 1 + argc + 1 + envc + 1 + 12;
    size_t table_bytes = table_elements * sizeof(uintptr_t);

    size_t total_pure_size = table_bytes + strings_size;
    uintptr_t final_user_rsp = (user_stack_top - total_pure_size) & ~0xFUL;
    size_t total_payload_size = user_stack_top - final_user_rsp;

    uintptr_t kbuf_top = (uintptr_t)kbuf + TMP_STACK_SIZE;
    uintptr_t curr_kbuf = kbuf_top - total_payload_size;
    
    uintptr_t *k_table = (uintptr_t *)curr_kbuf;
    char *k_strings_pos = (char *)(curr_kbuf + table_bytes);

    uintptr_t u_string_pos = final_user_rsp + table_bytes;

    k_table[0] = (uintptr_t)argc;
    int table_idx = 1;

    // argv
    size_t current_str_offset = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        memcpy(k_strings_pos + current_str_offset, argv[i], len);
        k_table[table_idx++] = u_string_pos + current_str_offset;
        current_str_offset += len;
    }
    k_table[table_idx++] = 0; 

    // envp
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        memcpy(k_strings_pos + current_str_offset, envp[i], len);
        k_table[table_idx++] = u_string_pos + current_str_offset;
        current_str_offset += len;
    }
    k_table[table_idx++] = 0; 

    k_table[table_idx++] = AT_PHDR;
    k_table[table_idx++] = phdr_vaddr;
    
    k_table[table_idx++] = AT_PHNUM;
    k_table[table_idx++] = phnum;
    
    k_table[table_idx++] = AT_PHENT;
    k_table[table_idx++] = sizeof(Elf64_Phdr);

    k_table[table_idx++] = AT_RANDOM;
    k_table[table_idx++] = user_stack_top - 16;
    
    k_table[table_idx++] = AT_PAGESZ;
    k_table[table_idx++] = 4096;
    
    k_table[table_idx++] = AT_NULL;
    k_table[table_idx++] = 0;

    uintptr_t copy_dest_u = final_user_rsp;
    size_t remain = total_payload_size;
    size_t offset_in_payload = 0;

    while (remain > 0) {
        uintptr_t phys = mmu_translate(new_map, copy_dest_u);
        size_t off_in_page = copy_dest_u % PAGE_SIZE;
        size_t to_copy = MIN(remain, PAGE_SIZE - off_in_page);

        void *src_addr = (void *)(curr_kbuf + offset_in_payload);
        void *dst_addr = (void *)p2v(phys);

        memcpy(dst_addr, src_addr, to_copy);

        remain -= to_copy;
        offset_in_payload += to_copy;
        copy_dest_u += to_copy;
    }

    kfree(kbuf);
    return final_user_rsp;
}