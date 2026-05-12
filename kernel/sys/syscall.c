#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>

#include <string.h>

typedef int64_t (*syscall_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

int copy_from_user(void *dest, const void *user_src, size_t n) {
    page_table_t *map = curthread->t_proc->p_vm_map;
    uintptr_t src = (uintptr_t)user_src;
    uint8_t *d = (uint8_t *)dest;
    size_t copied = 0;

    while (copied < n) {
        uintptr_t paddr = mmu_translate(map, src + copied);
        if (!paddr) return -1;

        size_t page_offset = (src + copied) & 0xFFF;
        size_t to_copy = 4096 - page_offset;
        if (to_copy > (n - copied)) to_copy = n - copied;

        memcpy(d + copied, (void *)p2v(paddr), to_copy);
        copied += to_copy;
    }
    return 0;
}

int64_t sys_test(uint64_t arg1) {
    dprintf("Syscall Test Success! Arg: 0x%lx\n", arg1);
    return 0xABCD;
}

int64_t sys_write(uint64_t fd, const char *buf, size_t count) {
    if (fd == 1 || fd == 2) {
        char kbuf[256];
        size_t processed = 0;

        while (processed < count) {
            size_t chunk = (count - processed > 255) ? 255 : (count - processed);
            
            if (copy_from_user(kbuf, buf + processed, chunk) < 0) {
                return -EFAULT;
            }

            for (size_t i = 0; i < chunk; i++) {
                kputc(kbuf[i]);
            }
            processed += chunk;
        }
        return count;
    }
    return -EBADF;
}

int64_t sys_brk(uintptr_t brk) {
    uintptr_t old_brk = curproc->p_brk;
    if (brk == 0) {
        return old_brk;
    }

    if (brk > old_brk) {
        uintptr_t start = ALIGN_UP(old_brk, PAGE_SIZE);
        uintptr_t end = ALIGN_UP(brk, PAGE_SIZE);

        if (end > curproc->p_stack_top) {
            return old_brk;
        }

        for (uintptr_t i = start; i < end; i += PAGE_SIZE) {
            if (mmu_translate(curproc->p_vm_map, i) == 0) {
                page_t* pg = page_alloc(0);
                if (!pg) {
                    return old_brk;
                }
                uintptr_t paddr = page_to_phys(pg);
                memset(p2v(paddr), 0, PAGE_SIZE);
                if (!mmu_map(curproc->p_vm_map, i, paddr, MMU_FLAGS_USER | MMU_FLAGS_WRITE)) {
                    page_free(pg, 0);
                    return old_brk; 
                }
            }
        }
    } else if (brk < old_brk) {
    }

    curproc->p_brk = brk;
    return brk;
}

static syscall_t syscall_table[] = {
    [0] = (syscall_t)sys_test,
    [1] = (syscall_t)sys_write,
    [2] = (syscall_t)sys_brk,
};

int64_t do_syscall_handler(struct trapframe *tf) {
    uint64_t num = tf->rax;
    uint64_t a1 = tf->rdi;
    uint64_t a2 = tf->rsi;
    uint64_t a3 = tf->rdx;
    uint64_t a4 = tf->r10;
    uint64_t a5 = tf->r8;

    if (num >= sizeof(syscall_table)/sizeof(syscall_t) || !syscall_table[num]) {
        return -ENOSYS; 
    }

    tf->rax = syscall_table[num](a1, a2, a3, a4, a5, 0);
    return tf->rax;
}