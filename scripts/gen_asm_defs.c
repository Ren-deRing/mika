#include <stdio.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>

int main() {
    printf("#define TRAPFRAME_SIZE %zu\n", sizeof(struct trapframe));

    printf("#define TF_RAX    0x%zx\n", offsetof(struct trapframe, rax));
    printf("#define TF_RBX    0x%zx\n", offsetof(struct trapframe, rbx));
    printf("#define TF_RCX    0x%zx\n", offsetof(struct trapframe, rcx));
    printf("#define TF_RDX    0x%zx\n", offsetof(struct trapframe, rdx));
    printf("#define TF_RSI    0x%zx\n", offsetof(struct trapframe, rsi));
    printf("#define TF_RDI    0x%zx\n", offsetof(struct trapframe, rdi));
    printf("#define TF_RBP    0x%zx\n", offsetof(struct trapframe, rbp));
    printf("#define TF_R8     0x%zx\n", offsetof(struct trapframe, r8));
    printf("#define TF_R9     0x%zx\n", offsetof(struct trapframe, r9));
    printf("#define TF_R10    0x%zx\n", offsetof(struct trapframe, r10));
    printf("#define TF_R11    0x%zx\n", offsetof(struct trapframe, r11));
    printf("#define TF_R12    0x%zx\n", offsetof(struct trapframe, r12));
    printf("#define TF_R13    0x%zx\n", offsetof(struct trapframe, r13));
    printf("#define TF_R14    0x%zx\n", offsetof(struct trapframe, r14));
    printf("#define TF_R15    0x%zx\n", offsetof(struct trapframe, r15));
    printf("#define TF_RIP    0x%zx\n", offsetof(struct trapframe, rip));
    printf("#define TF_CS     0x%zx\n", offsetof(struct trapframe, cs));
    printf("#define TF_RFLAGS 0x%zx\n", offsetof(struct trapframe, rflags));
    printf("#define TF_RSP    0x%zx\n", offsetof(struct trapframe, rsp));
    printf("#define TF_SS     0x%zx\n", offsetof(struct trapframe, ss));
    // printf("#define TF_ERR_CODE 0x%zx\n", offsetof(struct trapframe, err_code));

    printf("#define CPU_CURRENT_THREAD  0x%zx\n", offsetof(struct cpu, current_thread));
    printf("#define CPU_USER_RSP        0x%zx\n", offsetof(struct cpu, user_rsp));
    printf("#define CPU_KERNEL_RSP        0x%zx\n", offsetof(struct cpu, tss_rsp0));
    printf("#define KSTACK_SIZE           %d\n", KSTACK_SIZE);

    printf("#define THREAD_NEED_RESCHED 0x%zx\n", offsetof(struct thread, t_need_resched));
    printf("#define THREAD_CONTEXT      0x%zx\n", offsetof(struct thread, t_context));
    printf("#define THREAD_KSTACK       0x%zx\n", offsetof(struct thread, t_kstack));
    printf("#define THREAD_STATE        0x%zx\n", offsetof(struct thread, t_state));
    printf("#define THREAD_TRAPFRAME    0x%zx\n", offsetof(struct thread, t_trapframe));
    printf("#define THREAD_FS_BASE      0x%zx\n", offsetof(struct thread, t_fs_base));
    printf("#define THREAD_ARCH_DATA    0x%zx\n", offsetof(struct thread, t_arch_data));

    return 0;
}
