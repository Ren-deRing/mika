#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <string.h>

void handle_signal_dispatch(struct trapframe *tf, int sig, struct sigaction *act) {
    uintptr_t user_rsp = tf->rsp;

    uintptr_t frame_addr = (user_rsp - sizeof(struct sigframe)) & ~0xF;

    struct sigframe frame;
    memset(&frame, 0, sizeof(struct sigframe));

    memcpy(&frame.sf_tf, tf, sizeof(struct trapframe));
    frame.sf_oldmask = curthread->t_sig_mask;

    // 트램폴린 준비 (mov rax, 15; syscall)
    frame.sf_trampoline[0] = 0x48; // mov rax, 15
    frame.sf_trampoline[1] = 0xc7;
    frame.sf_trampoline[2] = 0xc0;
    frame.sf_trampoline[3] = 0x0f;
    frame.sf_trampoline[4] = 0x00;
    frame.sf_trampoline[5] = 0x00;
    frame.sf_trampoline[6] = 0x00;
    frame.sf_trampoline[7] = 0x0f; // syscall
    frame.sf_trampoline[8] = 0x05;

    if (copy_to_user((void *)frame_addr, &frame, sizeof(struct sigframe)) < 0) {
        sys_exit(128 + SIGSEGV);
        return;
    }

    tf->rip = (uintptr_t)act->sa_handler;
    tf->rdi = (uint64_t)sig;
    tf->rsp = frame_addr;

    uintptr_t ret_addr = act->sa_restorer ? (uintptr_t)act->sa_restorer : (frame_addr + offsetof(struct sigframe, sf_trampoline));
    tf->rsp -= 8;
    if (copy_to_user((void *)tf->rsp, &ret_addr, sizeof(uintptr_t)) < 0) {
        sys_exit(128 + SIGSEGV);
        return;
    }

    curthread->t_sig_mask |= act->sa_mask;
    curthread->t_in_sighandler = true;

    curthread->t_sig_pending &= ~(1ULL << (sig - 1));
}

void check_signals(struct trapframe *tf) {
    if (!curthread || !curthread->t_proc) return;
    if (curthread->t_tid == 0) return;

    if ((tf->cs & 3) != 3) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&curthread->t_proc->p_lock);

    uint64_t pending = curthread->t_sig_pending & ~curthread->t_sig_mask;
    if (pending == 0) {
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    }

    int sig = 0;
    for (int i = 0; i < NSIG; i++) {
        if (pending & (1ULL << i)) {
            sig = i + 1;
            break;
        }
    }

    if (sig == 0) {
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    }

    struct sigaction act = curthread->t_sig_actions[sig - 1];

    if (act.sa_handler == SIG_DFL) {
        if (sig == SIGKILL || sig == SIGTERM || sig == SIGINT || sig == SIGQUIT || sig == SIGHUP || 
            sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGBUS || sig == SIGABRT) {
            
            spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
            dprintf("[SIGNAL] Process %d killed by signal %d at RIP=0x%lx\n", curthread->t_proc->p_pid, sig, tf->rip);
            sys_exit(128 + sig);
            return;
        }
        curthread->t_sig_pending &= ~(1ULL << (sig - 1));
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    } else if (act.sa_handler == SIG_IGN) {
        curthread->t_sig_pending &= ~(1ULL << (sig - 1));
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        return;
    } else {
        spin_unlock_irqrestore(&curthread->t_proc->p_lock, flags);
        handle_signal_dispatch(tf, sig, &act);
        return;
    }
}

int64_t sys_rt_sigaction(int sig, const void *act, void *oact, size_t sigsetsize) {
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }
    if (sigsetsize != sizeof(sigset_t)) {
        return -EINVAL;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -EINVAL;
    }

    if (oact) {
        if (!is_user_address_range(oact, sizeof(struct sigaction))) {
            return -EFAULT;
        }
        if (copy_to_user(oact, &curthread->t_sig_actions[sig - 1], sizeof(struct sigaction)) < 0) {
            return -EFAULT;
        }
    }

    if (act) {
        if (!is_user_address_range(act, sizeof(struct sigaction))) {
            return -EFAULT;
        }
        struct sigaction new_act;
        if (copy_from_user(&new_act, act, sizeof(struct sigaction)) < 0) {
            return -EFAULT;
        }
        curthread->t_sig_actions[sig - 1] = new_act;
    }

    return 0;
}

int64_t sys_rt_sigreturn(void) {
    uintptr_t frame_addr = curthread->t_trapframe->rsp;

    struct sigframe frame;
    if (copy_from_user(&frame, (void *)frame_addr, sizeof(struct sigframe)) < 0) {
        sys_exit(128 + SIGSEGV);
        return -EFAULT;
    }

    if ((frame.sf_tf.cs & 3) != 3 || (frame.sf_tf.ss & 3) != 3) {
        dprintf("Attempted privilege escalation via sigreturn! PID: %d\n", curthread->t_proc->p_pid);
        sys_exit(128 + SIGSEGV);
        return -EPERM;
    }

    memcpy(curthread->t_trapframe, &frame.sf_tf, sizeof(struct trapframe));
    curthread->t_sig_mask = frame.sf_oldmask;
    curthread->t_in_sighandler = false;

    return curthread->t_trapframe->rax;
}

int64_t sys_kill(pid_t pid, int sig) {
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }

    struct proc *p = find_proc(pid);
    if (!p) {
        return -ESRCH;
    }

    struct thread *t = p->p_threads;
    if (!t) {
        proc_put(p);
        return -ESRCH;
    }

    uint64_t flags = spin_lock_irqsave(&p->p_lock);
    t->t_sig_pending |= (1ULL << (sig - 1));
    spin_unlock_irqrestore(&p->p_lock, flags);

    thread_signal_wakeup(t);

    proc_put(p);
    return 0;
}

int64_t sys_rt_sigprocmask(int how, const void *set, void *oset, size_t sigsetsize) {
    (void)how; (void)set; (void)sigsetsize;
    if (oset && is_user_address_range(oset, sigsetsize)) {
        uint64_t zero = 0;
        copy_to_user(oset, &zero, sizeof(zero));
    }
    return 0;
}

int64_t sys_tkill(int tid, int sig) {
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }

    struct proc *p = curproc;
    if (!p) return -ESRCH;

    uint64_t flags = spin_lock_irqsave(&p->p_lock);
    struct thread *t = p->p_threads;
    struct thread *target = NULL;

    while (t) {
        if (t->t_tid == tid) {
            target = t;
            break;
        }
        t = t->t_proc_next;
    }

    if (!target) {
        spin_unlock_irqrestore(&p->p_lock, flags);
        return -ESRCH;
    }

    target->t_sig_pending |= (1ULL << (sig - 1));
    spin_unlock_irqrestore(&p->p_lock, flags);

    thread_signal_wakeup(target);
    return 0;
}

int64_t sys_tgkill(int tgid, int tid, int sig) {
    struct proc *p = find_proc(tgid);
    if (!p) return -ESRCH;

    uint64_t flags = spin_lock_irqsave(&p->p_lock);
    struct thread *t = p->p_threads;
    struct thread *target = NULL;

    while (t) {
        if (t->t_tid == tid) {
            target = t;
            break;
        }
        t = t->t_proc_next;
    }

    if (!target) {
        spin_unlock_irqrestore(&p->p_lock, flags);
        proc_put(p);
        return -ESRCH;
    }

    target->t_sig_pending |= (1ULL << (sig - 1));
    spin_unlock_irqrestore(&p->p_lock, flags);

    thread_signal_wakeup(target);
    proc_put(p);
    return 0;
}

