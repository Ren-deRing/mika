.section .text

#include "build/gen/asm_defines.h"

.global syscall_entry
.extern do_syscall_handler

syscall_entry:
    swapgs

    movq %rsp, %gs:16 # 끼야악 모두 도망쳐요
    
    movq %gs:8, %rsp # 안녕하세요 커널입니다

    subq $TRAPFRAME_SIZE, %rsp   # sizeof(struct trapframe)
    movq %r11, 136(%rsp)         # TF->rflags
    movq %rcx, 120(%rsp)         # TF->rip
    movq %gs:16, %rax            # 죄송합니다 오발령이였습니다 모두 복귀하세요
    movq %rax, 144(%rsp)         # TF->rsp

    movq %rbx, 8(%rsp)
    movq %rdx, 24(%rsp)
    movq %rsi, 32(%rsp)
    movq %rdi, 40(%rsp)
    movq %rbp, 48(%rsp)
    movq %r8, 56(%rsp)
    movq %r9, 64(%rsp)
    movq %r10, 72(%rsp)
    movq %r11, 80(%rsp)
    movq %r12, 88(%rsp)
    movq %r13, 96(%rsp)
    movq %r14, 104(%rsp)
    movq %r15, 112(%rsp)

    movq %rsp, %rdi
    call do_syscall_handler

    movq %rsp, %rax
    andq $-8192, %rax # rax = struct thread_info*
    movq (%rax), %rax # rax = struct thread*

    cmpb $1, 64(%rax)
    jne .syscall_exit

    # 스케줄링은 나중에

.syscall_exit:
    movq 0(%rsp), %rax

    movq 8(%rsp), %rbx
    movq 24(%rsp), %rdx
    movq 32(%rsp), %rsi
    movq 40(%rsp), %rdi
    movq 48(%rsp), %rbp
    movq 56(%rsp), %r8
    movq 64(%rsp), %r9
    movq 72(%rsp), %r10
    movq 80(%rsp), %r11
    movq 88(%rsp), %r12
    movq 96(%rsp), %r13
    movq 104(%rsp), %r14
    movq 112(%rsp), %r15

    movq 120(%rsp), %rcx    # TF->rip -> rcx
    movq 136(%rsp), %r11    # TF->rflags -> r11
    movq 144(%rsp), %rsp    # TF->rsp -> rsp

    swapgs
    sysretq
