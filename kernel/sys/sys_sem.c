#include <uapi/sys/sem.h>
#include <uapi/errno.h>
#include <kernel/proc.h>
#include <kernel/lock.h>
#include <kernel/printf.h>
#include <kernel/list.h>
#include <kernel/sched.h>
#include <string.h>

#define MAX_SEM_SETS 32
#define MAX_SEMS_PER_SET 32
#define USER_ADDR_LIMIT  0x0000800000000000UL

struct sem_internal {
    uint16_t val;
    pid_t lpid;
};

struct sem_set_internal {
    int id;               // semid
    key_t key;
    size_t nsems;
    struct sem_internal sems[MAX_SEMS_PER_SET];
    struct semid_ds ds;
    struct list_node wait_queue;
    spinlock_t lock;
    bool active;
};

static struct sem_set_internal sem_sets[MAX_SEM_SETS];
static spinlock_t sem_global_lock = SPINLOCK_INITIALIZER;
static int next_semid = 1;

extern int copy_to_user(void *user_dest, const void *src, size_t n);
extern int copy_from_user(void *dest, const void *user_src, size_t n);

static inline bool is_user_address_range(const void *addr, size_t size) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    if (end < start) return false;
    if (end > USER_ADDR_LIMIT) return false;
    return true;
}

int64_t sys_semget(key_t key, int nsems, int semflg) {
    if (nsems < 0 || nsems > MAX_SEMS_PER_SET) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&sem_global_lock);

    if (key != IPC_PRIVATE) {
        for (int i = 0; i < MAX_SEM_SETS; i++) {
            if (sem_sets[i].active && sem_sets[i].key == key) {
                if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                    spin_unlock_irqrestore(&sem_global_lock, flags);
                    return -EEXIST;
                }
                if (nsems > 0 && (size_t)nsems > sem_sets[i].nsems) {
                    spin_unlock_irqrestore(&sem_global_lock, flags);
                    return -EINVAL;
                }
                int id = sem_sets[i].id;
                spin_unlock_irqrestore(&sem_global_lock, flags);
                return id;
            }
        }
    }

    if (!(semflg & IPC_CREAT) && key != IPC_PRIVATE) {
        spin_unlock_irqrestore(&sem_global_lock, flags);
        return -ENOENT;
    }

    if (nsems <= 0) {
        spin_unlock_irqrestore(&sem_global_lock, flags);
        return -EINVAL;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SEM_SETS; i++) {
        if (!sem_sets[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        spin_unlock_irqrestore(&sem_global_lock, flags);
        return -ENOMEM;
    }

    struct sem_set_internal *set = &sem_sets[slot];
    memset(set, 0, sizeof(*set));

    set->id = next_semid++;
    set->key = key;
    set->nsems = nsems;
    list_init(&set->wait_queue);
    spin_lock_init(&set->lock);
    set->active = true;

    set->ds.sem_perm.__key = key;
    set->ds.sem_perm.mode = semflg & 0777;
    set->ds.sem_perm.uid = curproc->p_uid;
    set->ds.sem_perm.gid = curproc->p_gid;
    set->ds.sem_perm.cuid = curproc->p_uid;
    set->ds.sem_perm.cgid = curproc->p_gid;
    set->ds.sem_nsems = nsems;
    set->ds.sem_ctime = 0;

    int id = set->id;
    spin_unlock_irqrestore(&sem_global_lock, flags);
    return id;
}

int64_t sys_semop(int semid, struct sembuf *sops, size_t nsops) {
    if (nsops == 0 || nsops > MAX_SEMS_PER_SET) return -EINVAL;
    if (!sops || !is_user_address_range(sops, nsops * sizeof(struct sembuf))) return -EFAULT;

    struct sembuf k_sops[MAX_SEMS_PER_SET];
    if (copy_from_user(k_sops, sops, nsops * sizeof(struct sembuf)) < 0) {
        return -EFAULT;
    }

    struct sem_set_internal *set = NULL;
    uint64_t global_flags = spin_lock_irqsave(&sem_global_lock);
    for (int i = 0; i < MAX_SEM_SETS; i++) {
        if (sem_sets[i].active && sem_sets[i].id == semid) {
            set = &sem_sets[i];
            break;
        }
    }
    spin_unlock_irqrestore(&sem_global_lock, global_flags);

    if (!set) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&set->lock);

    while (1) {
        if (!set->active) {
            spin_unlock_irqrestore(&set->lock, flags);
            return -EIDRM;
        }

        bool can_succeed = true;
        for (size_t i = 0; i < nsops; i++) {
            struct sembuf *op = &k_sops[i];
            if (op->sem_num >= set->nsems) {
                spin_unlock_irqrestore(&set->lock, flags);
                return -EFBIG;
            }

            int current_val = set->sems[op->sem_num].val;
            if (op->sem_op < 0) {
                if (current_val + op->sem_op < 0) {
                    can_succeed = false;
                    break;
                }
            } else if (op->sem_op == 0) {
                if (current_val != 0) {
                    can_succeed = false;
                    break;
                }
            }
        }

        if (can_succeed) {
            for (size_t i = 0; i < nsops; i++) {
                struct sembuf *op = &k_sops[i];
                set->sems[op->sem_num].val += op->sem_op;
                set->sems[op->sem_num].lpid = curproc->p_pid;
            }

            set->ds.sem_otime = 0;

            while (!list_empty(&set->wait_queue)) {
                struct thread *waiter = list_first_entry(&set->wait_queue, struct thread, t_wait_node);
                list_del(&waiter->t_wait_node);
                waiter->t_state = THREAD_READY;
                sched_enqueue(waiter);
            }

            spin_unlock_irqrestore(&set->lock, flags);
            return 0;
        }

        for (size_t i = 0; i < nsops; i++) {
            if (k_sops[i].sem_flg & IPC_NOWAIT) {
                spin_unlock_irqrestore(&set->lock, flags);
                return -EAGAIN;
            }
        }

        struct thread *t = curthread;
        t->t_state = THREAD_WAITING;
        t->t_lock_to_release = &set->lock;
        list_add_tail(&t->t_wait_node, &set->wait_queue);

        thread_yield();

        flags = spin_lock_irqsave(&set->lock);
    }
}

int64_t sys_semctl(int semid, int semnum, int cmd, uint64_t arg) {
    struct sem_set_internal *set = NULL;
    uint64_t global_flags = spin_lock_irqsave(&sem_global_lock);
    for (int i = 0; i < MAX_SEM_SETS; i++) {
        if (sem_sets[i].active && sem_sets[i].id == semid) {
            set = &sem_sets[i];
            break;
        }
    }
    spin_unlock_irqrestore(&sem_global_lock, global_flags);

    if (!set) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&set->lock);

    if (!set->active) {
        spin_unlock_irqrestore(&set->lock, flags);
        return -EINVAL;
    }

    switch (cmd) {
        case GETVAL:
            if (semnum < 0 || (size_t)semnum >= set->nsems) {
                spin_unlock_irqrestore(&set->lock, flags);
                return -EINVAL;
            }
            {
                int val = set->sems[semnum].val;
                spin_unlock_irqrestore(&set->lock, flags);
                return val;
            }

        case SETVAL:
            if (semnum < 0 || (size_t)semnum >= set->nsems) {
                spin_unlock_irqrestore(&set->lock, flags);
                return -EINVAL;
            }
            set->sems[semnum].val = (uint16_t)arg;
            set->sems[semnum].lpid = curproc->p_pid;

            while (!list_empty(&set->wait_queue)) {
                struct thread *waiter = list_first_entry(&set->wait_queue, struct thread, t_wait_node);
                list_del(&waiter->t_wait_node);
                waiter->t_state = THREAD_READY;
                sched_enqueue(waiter);
            }

            spin_unlock_irqrestore(&set->lock, flags);
            return 0;

        case IPC_RMID:
            set->active = false;
            while (!list_empty(&set->wait_queue)) {
                struct thread *waiter = list_first_entry(&set->wait_queue, struct thread, t_wait_node);
                list_del(&waiter->t_wait_node);
                waiter->t_state = THREAD_READY;
                if (waiter->t_trapframe) {
                    waiter->t_trapframe->rax = -EIDRM;
                }
                sched_enqueue(waiter);
            }
            spin_unlock_irqrestore(&set->lock, flags);
            return 0;

        case IPC_STAT:
            {
                struct semid_ds *buf = (struct semid_ds *)arg;
                if (!buf || !is_user_address_range(buf, sizeof(struct semid_ds))) {
                    spin_unlock_irqrestore(&set->lock, flags);
                    return -EFAULT;
                }
                if (copy_to_user(buf, &set->ds, sizeof(struct semid_ds)) < 0) {
                    spin_unlock_irqrestore(&set->lock, flags);
                    return -EFAULT;
                }
                spin_unlock_irqrestore(&set->lock, flags);
                return 0;
            }

        default:
            spin_unlock_irqrestore(&set->lock, flags);
            return -EINVAL;
    }
}
