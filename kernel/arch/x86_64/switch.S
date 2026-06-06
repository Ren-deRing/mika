.section .text

#include "build/gen/asm_defines.h"

.global arch_start_thread
.global arch_start_ap_thread
.global arch_context_switch
.global thread_bootstrap
.global arch_enter_user_mode
.global fork_child_ret

.extern generic_main
.extern ap_main
.extern g_use_xsave
.extern thread_post_switch_hook

arch_start_thread:
    movq 16(%rdi), %rsp # rsp = t0->t_kstack
    addq $8192, %rsp    # rsp += KSTACK_SIZE

    jmp generic_main

arch_start_ap_thread:
    movq 16(%rdi), %rsp # rsp = t->t_kstack
    addq $8192, %rsp    # rsp += KSTACK_SIZE

    jmp ap_main


arch_context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq THREAD_ARCH_DATA(%rdi), %rcx
    testq %rcx, %rcx
    jz .Lno_xsave
    cmpb $0, g_use_xsave(%rip)
    jz .Luse_fxsave
    movl $0xFFFFFFFF, %eax
    movl $0xFFFFFFFF, %edx
    xsaveq (%rcx)
    jmp .Lno_xsave
.Luse_fxsave:
    fxsave (%rcx)
.Lno_xsave:

    movq %rsp, 24(%rdi)

    movq 24(%rsi), %rsp

    movq THREAD_ARCH_DATA(%rsi), %rcx
    testq %rcx, %rcx
    jz .Lno_xrstor
    cmpb $0, g_use_xsave(%rip)
    jz .Luse_fxrstor
    movl $0xFFFFFFFF, %eax
    movl $0xFFFFFFFF, %edx
    xrstorq (%rcx)
    jmp .Lno_xrstor
.Luse_fxrstor:
    fxrstor (%rcx)
.Lno_xrstor:

    # Restore FS_BASE
    movq THREAD_FS_BASE(%rsi), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl %eax, %eax
    movl $0xC0000100, %ecx
    wrmsr

    movq THREAD_TRAPFRAME(%rsi), %rax
    cmpq %rax, %rsp
    je arch_return_to_user

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    ret

    

thread_bootstrap:
    call thread_post_switch_hook

    sti

    movq %r13, %rdi
    movq %r12, %rax

    call *%rax

.Lhang:
    cli
    hlt
    jmp .Lhang

arch_enter_user_mode:  # rdi=entry, rsi=user_rsp
    cli

    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es

    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rbp, %rbp
    xorq %r8,  %r8
    xorq %r9,  %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15

    pushq $0x23     # ss
    pushq %rsi      # user rsp
    pushq $0x202    # rflags
    pushq $0x2B     # cs
    pushq %rdi      # rip

    swapgs
    iretq

fork_child_ret:
    call thread_post_switch_hook
    call fork_child_switch_mm
    
    movq %gs:CPU_CURRENT_THREAD, %rax
    movq THREAD_TRAPFRAME(%rax), %rsp

    movq TF_RAX(%rsp), %rax
    movq TF_RBX(%rsp), %rbx
    movq TF_RDX(%rsp), %rdx
    movq TF_RSI(%rsp), %rsi
    movq TF_RDI(%rsp), %rdi
    movq TF_RBP(%rsp), %rbp
    movq TF_R8(%rsp),  %r8
    movq TF_R9(%rsp),  %r9
    movq TF_R10(%rsp), %r10
    movq TF_R12(%rsp), %r12
    movq TF_R13(%rsp), %r13
    movq TF_R14(%rsp), %r14
    movq TF_R15(%rsp), %r15

    movq %rsp, %r11

    movq TF_SS(%r11), %r10
    pushq %r10
    movq TF_RSP(%r11), %r10
    pushq %r10
    movq TF_RFLAGS(%r11), %r10
    pushq %r10
    movq TF_CS(%r11), %r10
    pushq %r10
    movq TF_RIP(%r11), %r10
    pushq %r10

    swapgs
    iretq

.global arch_return_to_user
arch_return_to_user:
    movq %gs:CPU_CURRENT_THREAD, %rax
    movq THREAD_TRAPFRAME(%rax), %rsp

    movq TF_RAX(%rsp), %rax
    movq TF_RBX(%rsp), %rbx
    movq TF_RDX(%rsp), %rdx
    movq TF_RSI(%rsp), %rsi
    movq TF_RDI(%rsp), %rdi
    movq TF_RBP(%rsp), %rbp
    movq TF_R8(%rsp),  %r8
    movq TF_R9(%rsp),  %r9
    movq TF_R10(%rsp), %r10
    movq TF_R12(%rsp), %r12
    movq TF_R13(%rsp), %r13
    movq TF_R14(%rsp), %r14
    movq TF_R15(%rsp), %r15

    movq %rsp, %r11

    movq TF_SS(%r11), %r10
    pushq %r10
    movq TF_RSP(%r11), %r10
    pushq %r10
    movq TF_RFLAGS(%r11), %r10
    pushq %r10
    movq TF_CS(%r11), %r10
    pushq %r10
    movq TF_RIP(%r11), %r10
    pushq %r10

    swapgs
    iretq