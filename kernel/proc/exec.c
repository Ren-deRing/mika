#include <kernel/exec.h>
#include <kernel/mmu.h>
#include <kernel/kmem.h>
#include <kernel/proc.h>
#include <kernel/vma.h>
#include <kernel/cpu.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

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
    arch_enter_user_mode(p->p_entry, self->t_user_stack_top);
}

static int copy_user_strlen(const char *user_str) {
    int len = 0;
    char c;
    while (len < 4096) {
        if (copy_from_user(&c, user_str + len, 1) < 0) return -1;
        if (c == '\0') return len;
        len++;
    }
    return len;
}

static int copy_user_string(char *kdest, const char *user_str, size_t max) {
    size_t i = 0;
    char c;
    while (i < max - 1) {
        if (copy_from_user(&c, user_str + i, 1) < 0) return -1;
        kdest[i] = c;
        if (c == '\0') return 0;
        i++;
    }
    kdest[i] = '\0';
    return 0;
}

int proc_exec(void *elf_data, size_t elf_size, char *const argv[], char *const envp[]) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    uintptr_t main_binary_base = (ehdr->e_type == ET_DYN) ? 0x00400000 : 0;
    uintptr_t original_entry = ehdr->e_entry + main_binary_base;
    uintptr_t entry_point = 0;
    uintptr_t brk = 0;
    uintptr_t phdr_vaddr = 0;
    uint64_t phnum = 0;
    uintptr_t interpreter_base = 0;

    page_table_t *new_map = load_elf(elf_data, elf_size, &entry_point, &brk, &phdr_vaddr, &phnum, &interpreter_base);
    if (!new_map) {
        return -ENOEXEC;
    }

    struct proc *p = proc_create(alloc_pid());
    if (!p) {
        mmu_destroy_map(new_map);
        return -ENOMEM;
    }

    uintptr_t stack_top = USER_STACK_TOP;
    uintptr_t stack_bottom = stack_top - USER_STACK_SIZE;

    struct vm_area *stack_vma = vma_alloc(stack_bottom, stack_top,
        MMU_FLAGS_USER | MMU_FLAGS_READ | MMU_FLAGS_WRITE, NULL, 0);
    if (stack_vma) {
        down_write(&p->p_vma_lock);
        vma_insert(&p->p_vma_root, &p->p_vma_list, stack_vma);
        up_write(&p->p_vma_lock);
    }

    uintptr_t final_rsp = setup_user_stack(new_map, USER_STACK_TOP, argv, envp, phdr_vaddr, phnum, interpreter_base, original_entry);

    page_table_t *old_map = p->p_vm_map;
    p->p_vm_map = new_map;
    {
        page_t *pg = phys_to_page(virt_to_phys(new_map));
        if (pg) pg->pg_proc = p;
    }
    if (old_map) mmu_destroy_map(old_map);
    p->p_entry = entry_point;
    p->p_stack_top = USER_STACK_TOP;
    p->p_brk = ALIGN_UP(brk, PAGE_SIZE);

    struct thread *new_t = thread_create(p, alloc_tid(), arch_user_trampoline, (void *)p);
    if (!new_t) {
        proc_free(p);
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
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_SECURE  23
#define AT_RANDOM 25

uintptr_t setup_user_stack(page_table_t *new_map, uintptr_t user_stack_top, 
                           char *const argv[], char *const envp[],
                           uintptr_t phdr_vaddr, uint64_t phnum,
                           uintptr_t interpreter_base, uintptr_t original_entry) {
    uint8_t *kbuf = kmalloc(TMP_STACK_SIZE);
    if (!kbuf) return 0;
    memset(kbuf, 0, TMP_STACK_SIZE);

    int argc = 0;
    if (argv) {
        while (1) {
            char *ptr;
            if (copy_from_user(&ptr, &argv[argc], sizeof(char *)) < 0) { kfree(kbuf); return 0; }
            if (!ptr) break;
            argc++;
        }
    }
    int envc = 0;
    if (envp) {
        while (1) {
            char *ptr;
            if (copy_from_user(&ptr, &envp[envc], sizeof(char *)) < 0) { kfree(kbuf); return 0; }
            if (!ptr) break;
            envc++;
        }
    }

    size_t strings_size = 0;
    char *k_argv[256];
    char *k_envp[256];
    if (argc > 255) argc = 255;
    if (envc > 255) envc = 255;

    for (int i = 0; i < argc; i++) {
        char *user_ptr;
        copy_from_user(&user_ptr, &argv[i], sizeof(char *));
        int slen = copy_user_strlen(user_ptr);
        if (slen < 0) { kfree(kbuf); return 0; }
        k_argv[i] = kmalloc(slen + 1);
        if (!k_argv[i]) { kfree(kbuf); return 0; }
        copy_user_string(k_argv[i], user_ptr, slen + 1);
        strings_size += slen + 1;
    }
    for (int i = 0; i < envc; i++) {
        char *user_ptr;
        copy_from_user(&user_ptr, &envp[i], sizeof(char *));
        int slen = copy_user_strlen(user_ptr);
        if (slen < 0) { kfree(kbuf); return 0; }
        k_envp[i] = kmalloc(slen + 1);
        if (!k_envp[i]) { kfree(kbuf); return 0; }
        copy_user_string(k_envp[i], user_ptr, slen + 1);
        strings_size += slen + 1;
    }

    size_t table_elements = 1 + argc + 1 + envc + 1 + 32;
    size_t table_bytes = table_elements * sizeof(uintptr_t);

    size_t total_pure_size = table_bytes + strings_size;
    if (total_pure_size > TMP_STACK_SIZE) {
        for (int i = 0; i < argc; i++) kfree(k_argv[i]);
        for (int i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(kbuf);
        return 0;
    }
    uintptr_t final_user_rsp = (user_stack_top - total_pure_size) & ~0xFUL;
    size_t total_payload_size = user_stack_top - final_user_rsp;

    uintptr_t kbuf_top = (uintptr_t)kbuf + TMP_STACK_SIZE;
    uintptr_t curr_kbuf = kbuf_top - total_payload_size;
    
    uintptr_t *k_table = (uintptr_t *)curr_kbuf;
    char *k_strings_pos = (char *)(curr_kbuf + table_bytes);

    uintptr_t u_string_pos = final_user_rsp + table_bytes;

    k_table[0] = (uintptr_t)argc;
    int table_idx = 1;

    size_t current_str_offset = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(k_argv[i]) + 1;
        memcpy(k_strings_pos + current_str_offset, k_argv[i], len);
        k_table[table_idx++] = u_string_pos + current_str_offset;
        current_str_offset += len;
        kfree(k_argv[i]);
    }
    k_table[table_idx++] = 0; 

    for (int i = 0; i < envc; i++) {
        size_t len = strlen(k_envp[i]) + 1;
        memcpy(k_strings_pos + current_str_offset, k_envp[i], len);
        k_table[table_idx++] = u_string_pos + current_str_offset;
        current_str_offset += len;
        kfree(k_envp[i]);
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

    if (interpreter_base != 0) {
        k_table[table_idx++] = AT_BASE;
        k_table[table_idx++] = interpreter_base;

        k_table[table_idx++] = AT_ENTRY;
        k_table[table_idx++] = original_entry;
    }
    
    k_table[table_idx++] = AT_UID;
    k_table[table_idx++] = 0;

    k_table[table_idx++] = AT_EUID;
    k_table[table_idx++] = 0;

    k_table[table_idx++] = AT_GID;
    k_table[table_idx++] = 0;

    k_table[table_idx++] = AT_EGID;
    k_table[table_idx++] = 0;

    k_table[table_idx++] = AT_SECURE;
    k_table[table_idx++] = 0;
    
    k_table[table_idx++] = AT_NULL;
    k_table[table_idx++] = 0;
    uintptr_t copy_dest_u = final_user_rsp;
    size_t remain = total_payload_size;
    size_t offset_in_payload = 0;

    while (remain > 0) {
        uintptr_t phys = mmu_translate(new_map, copy_dest_u);
        if (phys == 0) {
            page_t *pg = page_alloc(0);
            if (!pg) { kfree(kbuf); return 0; }
            phys = page_to_phys(pg);
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            mmu_map_4k(new_map, copy_dest_u & ~(PAGE_SIZE - 1), phys,
                MMU_FLAGS_USER | MMU_FLAGS_READ | MMU_FLAGS_WRITE);
        }
        size_t off_in_page = copy_dest_u % PAGE_SIZE;
        size_t to_copy = MIN(remain, PAGE_SIZE - off_in_page);

        void *src_addr = (void *)(curr_kbuf + offset_in_payload);
        void *dst_addr = (void *)(phys_to_virt(phys & ~(PAGE_SIZE - 1)) + off_in_page);

        memcpy(dst_addr, src_addr, to_copy);
        mmu_flush_cache(dst_addr, to_copy);

        remain -= to_copy;
        offset_in_payload += to_copy;
        copy_dest_u += to_copy;
    }

    kfree(kbuf);
    return final_user_rsp;
}