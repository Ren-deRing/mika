.section .text
.global arch_start_thread
.global arch_context_switch
.global thread_bootstrap
.global arch_enter_user_mode
.extern generic_main

arch_start_thread:
    movq 16(%rdi), %rsp # rsp = t0->t_kstack
    addq $8192, %rsp    # rsp += KSTACK_SIZE

    jmp generic_main

arch_context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, 24(%rdi)

    movq 24(%rsi), %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    ret

thread_bootstrap:
    sti

    movq %gs:0, %rax
    movq 32(%rax), %rbx # rbx = curthread

    movq 112(%rbx), %rdi    # arg1 = curthread->t_arg
    movq 104(%rbx), %rax    # rax = curthread->t_entry
    call *%rax

    # TODO: thread_exit()
.Lhang:
    cli
    hlt
    jmp .Lhang

arch_enter_user_mode:  // rdi=entry, rsi=user_rsp
    cli

    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es

    pushq $0x23     // ss
    pushq %rsi      // user rsp
    pushq $0x202    // rflags
    pushq $0x2B     // cs
    pushq %rdi      // rip

    swapgs
    iretq