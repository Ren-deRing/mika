#pragma once

#include <uapi/types.h>
#include <stdbool.h>

#include <kernel/cpu.h>
#include <kernel/mmu.h>
#include <kernel/fs/file.h>
#include <kernel/lock.h>

#include <uapi/types.h>
#include <uapi/signal.h>

#define MAX_FILES 256


#define THREAD_FLAG_USER   (1 << 0)
#define THREAD_FLAG_KERNEL (1 << 1)

#define SIG_DFL          ((void (*)(int))0)
#define SIG_IGN          ((void (*)(int))1)

typedef uint64_t sigset_t;

struct sigaction {
    void     (*sa_handler)(int);
    sigset_t   sa_mask;
    int        sa_flags;
    void     (*sa_restorer)(void);
};

extern pid_t next_pid;
extern tid_t next_tid;

typedef enum {
    THREAD_EMBRYO,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEP,
    THREAD_WAITING,
    THREAD_ZOMBIE
} thread_state_t;

typedef enum {
    PROC_RUNNING,
    PROC_ZOMBIE,
    PROC_STOPPED,
} proc_state_t;

struct proc;
struct vnode;
struct arch_proc;


struct sigframe {
    struct trapframe sf_tf;
    sigset_t         sf_oldmask;
    uint8_t          sf_trampoline[16];
} __attribute__((packed));

// TCB
struct thread {
    tid_t            t_tid;
    struct proc     *t_proc;

    void            *t_kstack;
    void            *t_context;
    void            *t_arch_data;
    struct trapframe *t_trapframe; 

    int              t_state;

    struct thread   *t_sched_next;
    bool             t_need_resched;
    uint32_t         t_ticks;

    int              t_priority;
    uint32_t         t_slice_left;
    uint32_t         t_cpu;

    uint64_t         t_sleep_until;
    struct list_node t_wait_node;
    spinlock_t      *t_lock_to_release;

    uintptr_t        t_fs_base;

    sigset_t         t_sig_pending;
    sigset_t         t_sig_mask;
    struct sigaction t_sig_actions[NSIG];
    bool             t_in_sighandler;

    void            (*t_entry)(void *);
    void            *t_arg;
    int              t_flags;

    uintptr_t        t_user_stack_top;

    int              t_errno;
    struct thread   *t_proc_next;
    int             *t_clear_child_tid;
    uintptr_t        t_futex_addr;
};

// PCB
struct proc {
    pid_t           p_pid;
    uid_t           p_uid, p_euid;
    gid_t           p_gid, p_egid;
    proc_state_t    p_state;
    spinlock_t      p_lock;

    char            p_name[16];
    mode_t          p_umask;

    struct file    *p_fd_table[MAX_FILES];
    struct vnode   *p_cwd;

    page_table_t   *p_vm_map;
    uintptr_t       p_entry;
    uintptr_t       p_stack_top;
    uintptr_t       p_brk;
    uintptr_t       p_mmap_base;

    struct arch_proc *p_arch;

    struct thread    *p_threads;

    struct proc      *p_parent;
    struct list_node  p_child_link;
    struct list_node  p_children;
    int               p_exit_status;
    struct list_node  p_wait_queue;

    int               p_refcnt;
    struct list_node  p_list_link;
};

struct proc* proc_create(pid_t pid);
void proc_free(struct proc *p);
struct thread* thread_create(struct proc *p, tid_t tid, void (*entry)(void *), void *arg);

int proc_alloc_fd(struct proc *p, struct file *f);

void proc_ref(struct proc *p);
void proc_put(struct proc *p);
struct proc* find_proc(pid_t pid);

void check_signals(struct trapframe *tf);