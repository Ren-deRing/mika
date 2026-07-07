.section .text

#include "build/gen/asm_defines.h"

.global syscall_entry
.extern do_syscall_handler
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP
    movq %gs:CPU_KERNEL_RSP, %rsp
    subq $TRAPFRAME_SIZE, %rsp
    movq %rax, TF_RAX(%rsp)
    movq %rbx, TF_RBX(%rsp)
    movq %rdx, TF_RDX(%rsp)
    movq %rsi, TF_RSI(%rsp)
    movq %rdi, TF_RDI(%rsp)
    movq %rbp, TF_RBP(%rsp)
    movq %r8, TF_R8(%rsp)
    movq %r9, TF_R9(%rsp)
    movq %r10, TF_R10(%rsp)
    movq %r12, TF_R12(%rsp)
    movq %r13, TF_R13(%rsp)
    movq %r14, TF_R14(%rsp)
    movq %r15, TF_R15(%rsp)
    movq %rcx, TF_RIP(%rsp)
    movq $0x2B, TF_CS(%rsp)
    movq $0x23, TF_SS(%rsp)
    movq %r11, TF_RFLAGS(%rsp)

    movq %gs:CPU_USER_RSP, %rax
    movq %rax, TF_RSP(%rsp)
    movq %rsp, %rdi
    
    call do_syscall_handler
    movq %gs:CPU_CURRENT_THREAD, %rcx
    cmpb $1, THREAD_NEED_RESCHED(%rcx)
    je .syscall_need_resched
    
    jmp .syscall_exit

.syscall_need_resched:
    movq %gs:CPU_CURRENT_THREAD, %rcx
    movb $0, THREAD_NEED_RESCHED(%rcx)
    call mi_switch
    jmp .syscall_exit
    
.global .syscall_exit
.syscall_exit:
    cli
    movq TF_RAX(%rsp), %rax
    movq TF_RBX(%rsp), %rbx
    movq TF_RDX(%rsp), %rdx
    movq TF_RSI(%rsp), %rsi
    movq TF_RDI(%rsp), %rdi
    movq TF_RBP(%rsp), %rbp
    movq TF_R8(%rsp), %r8
    movq TF_R9(%rsp), %r9
    movq TF_R10(%rsp), %r10
    movq TF_R12(%rsp), %r12
    movq TF_R13(%rsp), %r13
    movq TF_R14(%rsp), %r14
    movq TF_R15(%rsp), %r15
    movq TF_RIP(%rsp), %rcx
    movq TF_RFLAGS(%rsp), %r11
    movq TF_RSP(%rsp), %rsp
    swapgs
    sysretq