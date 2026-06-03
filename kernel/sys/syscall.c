#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <string.h>

#define USER_ADDR_LIMIT  0x0000800000000000UL

typedef int64_t (*syscall_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

bool is_user_address_range(const void *addr, size_t size) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;

    if (end < start) return false;
    if (end > USER_ADDR_LIMIT) return false;

    return true;
}

int copy_to_user(void *user_dest, const void *src, size_t n) {
    if (!curthread || !curthread->t_proc || !curthread->t_proc->p_vm_map) return -1;
    page_table_t *map = curthread->t_proc->p_vm_map;
    uintptr_t dest = (uintptr_t)user_dest;
    uint8_t *s = (uint8_t *)src;
    size_t copied = 0;

    while (copied < n) {
        uintptr_t paddr = mmu_translate(map, dest + copied);
        if (!paddr) return -1;

        size_t page_offset = (dest + copied) & 0xFFF;
        size_t to_copy = 4096 - page_offset;
        if (to_copy > (n - copied)) to_copy = n - copied;

        memcpy((void *)p2v(paddr), s + copied, to_copy);
        copied += to_copy;
    }
    return 0;
}

int copy_from_user(void *dest, const void *user_src, size_t n) {
    if (!curthread || !curthread->t_proc || !curthread->t_proc->p_vm_map) return -1;
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

static syscall_t syscall_table[] = {
    [0] = (syscall_t)sys_read,
    [1] = (syscall_t)sys_write,
    [2] = (syscall_t)sys_open,
    [3] = (syscall_t)sys_close,
    [5] = (syscall_t)sys_fstat,
    [8] = (syscall_t)sys_lseek,
    [9]  = (syscall_t)sys_mmap,
    [10] = (syscall_t)sys_mprotect,
    [11] = (syscall_t)sys_munmap,
    [12] = (syscall_t)sys_brk,
    [13] = (syscall_t)sys_rt_sigaction,
    [14] = (syscall_t)sys_rt_sigprocmask,
    [15] = (syscall_t)sys_rt_sigreturn,
    [16] = (syscall_t)sys_ioctl,
    [19] = (syscall_t)sys_readv,
    [20] = (syscall_t)sys_writev,
    [29] = (syscall_t)sys_shmget,
    [30] = (syscall_t)sys_shmat,
    [31] = (syscall_t)sys_shmctl,
    [39] = (syscall_t)sys_getpid,
    [56] = (syscall_t)sys_clone,
    [57] = (syscall_t)sys_fork,
    [59] = (syscall_t)sys_execve,
    [60] = (syscall_t)sys_exit,
    [61] = (syscall_t)sys_wait4,
    [62] = (syscall_t)sys_kill,
    [64] = (syscall_t)sys_semget,
    [65] = (syscall_t)sys_semop,
    [66] = (syscall_t)sys_semctl,
    [67] = (syscall_t)sys_shmdt,
    [72] = (syscall_t)sys_fcntl,
    [97]  = (syscall_t)sys_getrlimit,
    [158] = (syscall_t)sys_arch_prctl,
    [202] = (syscall_t)sys_futex,
    [218] = (syscall_t)sys_set_tid_address,
    [228] = (syscall_t)sys_clock_gettime,
    [231] = (syscall_t)sys_exit, // sys_exit_group
    [262] = (syscall_t)sys_newfstatat,
};

int64_t do_syscall_handler(struct trapframe *tf) {
    curthread->t_trapframe = tf;

    uint64_t num = tf->rax;

    uint64_t a1 = tf->rdi;
    uint64_t a2 = tf->rsi;
    uint64_t a3 = tf->rdx;
    uint64_t a4 = tf->r10;
    uint64_t a5 = tf->r8;
    uint64_t a6 = tf->r9;

    if (num >= sizeof(syscall_table)/sizeof(syscall_t) ||
        !syscall_table[num]) {
        tf->rax = -ENOSYS;
        return tf->rax;
    }

    int64_t ret =
        syscall_table[num](a1, a2, a3, a4, a5, a6);

    tf->rax = ret;

    check_signals(tf);

    return ret;
}