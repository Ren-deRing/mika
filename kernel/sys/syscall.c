#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <kernel/cpu.h>
#include <string.h>

#define USER_ADDR_LIMIT  arch_get_user_addr_limit()

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

        memcpy((void *)phys_to_virt(paddr), s + copied, to_copy);
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

        memcpy(d + copied, (void *)phys_to_virt(paddr), to_copy);
        copied += to_copy;
    }
    return 0;
}

int copy_str_from_user(char *dest, const char *user_src, size_t max_len) {
    if (!user_src) return -1;
    if (!curthread || !curthread->t_proc || !curthread->t_proc->p_vm_map) return -1;
    page_table_t *map = curthread->t_proc->p_vm_map;
    uintptr_t src = (uintptr_t)user_src;
    size_t copied = 0;

    while (copied < max_len) {
        uintptr_t paddr = mmu_translate(map, src + copied);
        if (!paddr) return -1;

        size_t page_offset = (src + copied) & 0xFFF;
        size_t to_copy = 4096 - page_offset;
        if (to_copy > (max_len - copied)) to_copy = max_len - copied;

        const char *src_ptr = (const char *)phys_to_virt(paddr);
        for (size_t i = 0; i < to_copy; i++) {
            dest[copied] = src_ptr[i];
            if (dest[copied] == '\0') {
                return 0;
            }
            copied++;
        }
    }

    if (max_len > 0) {
        dest[max_len - 1] = '\0';
    }
    return 0;
}

static syscall_t syscall_table[] = {
    [0] = (syscall_t)sys_read,
    [1] = (syscall_t)sys_write,
    [2] = (syscall_t)sys_open,
    [3] = (syscall_t)sys_close,
    [4] = (syscall_t)sys_stat,
    [5] = (syscall_t)sys_fstat,
    [6] = (syscall_t)sys_lstat,
    [7] = (syscall_t)sys_poll,
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
    [21] = (syscall_t)sys_access,
    [22] = (syscall_t)sys_pipe,
    [23] = (syscall_t)sys_select,
    [25] = (syscall_t)sys_mremap,
    [29] = (syscall_t)sys_shmget,
    [30] = (syscall_t)sys_shmat,
    [31] = (syscall_t)sys_shmctl,
    [35] = (syscall_t)sys_nanosleep,
    [39] = (syscall_t)sys_getpid,
    [41] = (syscall_t)sys_socket,
    [42] = (syscall_t)sys_connect,
    [43] = (syscall_t)sys_accept,
    [44] = (syscall_t)sys_sendto,
    [45] = (syscall_t)sys_recvfrom,
    [46] = (syscall_t)sys_sendmsg,
    [47] = (syscall_t)sys_recvmsg,
    [49] = (syscall_t)sys_bind,
    [50] = (syscall_t)sys_listen,
    [53] = (syscall_t)sys_socketpair,
    [54] = (syscall_t)sys_setsockopt,
    [55] = (syscall_t)sys_lchown,
    [56] = (syscall_t)sys_clone,
    [57] = (syscall_t)sys_fork,
    [59] = (syscall_t)sys_execve,
    [60] = (syscall_t)sys_exit,
    [61] = (syscall_t)sys_wait4,
    [62] = (syscall_t)sys_kill,
    [63] = (syscall_t)sys_uname,
    [64] = (syscall_t)sys_semget,
    [65] = (syscall_t)sys_semop,
    [66] = (syscall_t)sys_semctl,
    [67] = (syscall_t)sys_shmdt,
    [72] = (syscall_t)sys_fcntl,
    [73] = (syscall_t)sys_flock,
    [77] = (syscall_t)sys_ftruncate,
    [82] = (syscall_t)sys_rename,
    [83] = (syscall_t)sys_mkdir,
    [84] = (syscall_t)sys_rmdir,
    [87] = (syscall_t)sys_unlink,
    [88] = (syscall_t)sys_symlink,
    [89] = (syscall_t)sys_readlink,
    [97]  = (syscall_t)sys_getrlimit,
    [102] = (syscall_t)sys_getuid,
    [104] = (syscall_t)sys_getgid,
    [107] = (syscall_t)sys_geteuid,
    [108] = (syscall_t)sys_getegid,
    [112] = (syscall_t)sys_setsid,
    [117] = (syscall_t)sys_setresuid,
    [158] = (syscall_t)sys_arch_prctl,
    [165] = (syscall_t)sys_mount,
    [200] = (syscall_t)sys_tkill,
    [201] = (syscall_t)sys_time,
    [202] = (syscall_t)sys_futex,
    [217] = (syscall_t)sys_getdents64,
    [218] = (syscall_t)sys_set_tid_address,
    [222] = (syscall_t)sys_timer_create,
    [223] = (syscall_t)sys_timer_settime,
    [226] = (syscall_t)sys_timer_delete,
    [228] = (syscall_t)sys_clock_gettime,
    [229] = (syscall_t)sys_clock_getres,
    [231] = (syscall_t)sys_exit, // sys_exit_group
    [232] = (syscall_t)sys_epoll_wait,
    [233] = (syscall_t)sys_epoll_ctl,
    [234] = (syscall_t)sys_tgkill,
    [262] = (syscall_t)sys_newfstatat,
    [269] = (syscall_t)sys_faccessat,
    [273] = (syscall_t)sys_eventfd,
    [281] = (syscall_t)sys_epoll_pwait,
    [282] = (syscall_t)sys_signalfd,
    [283] = (syscall_t)sys_timerfd_create,
    [285] = (syscall_t)sys_fallocate,
    [286] = (syscall_t)sys_timerfd_settime,
    [287] = (syscall_t)sys_timerfd_gettime,
    [288] = (syscall_t)sys_accept4,
    [289] = (syscall_t)sys_signalfd4,
    [290] = (syscall_t)sys_eventfd2,
    [291] = (syscall_t)sys_epoll_create1,
    [293] = (syscall_t)sys_pipe2,
    [318] = (syscall_t)sys_getrandom,
    [319] = (syscall_t)sys_memfd_create,
    [324] = (syscall_t)sys_membarrier,
    [439] = (syscall_t)sys_faccessat2,
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
        dprintf("[SYSCALL] Unimplemented syscall %d called from RIP %p, args: %lx, %lx, %lx, %lx, %lx, %lx\n", 
                num, tf->rip, a1, a2, a3, a4, a5, a6);
        tf->rax = -ENOSYS;
        return tf->rax;
    }

    arch_irq_enable();

    int64_t ret =
        syscall_table[num](a1, a2, a3, a4, a5, a6);

    arch_irq_disable();

    tf->rax = ret;

    check_signals(tf);

    return ret;
}