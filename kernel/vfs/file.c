#include <kernel/fs/file.h>
#include <kernel/lock.h>
#include <kernel/rcu.h>
#include <kernel/kmem.h>
#include <string.h>

static void file_free_rcu(void *arg) {
    struct file *f = (struct file *)arg;
    kfree(f);
}

struct file *file_alloc(void) {
    struct file *f = kmalloc(sizeof(struct file));
    if (!f) return NULL;

    memset(f, 0, sizeof(struct file));
    f->f_refcnt = 1;
    spin_lock_init(&f->f_lock);

    return f;
}

void file_ref(struct file *f) {
    if (f) {
        __atomic_fetch_add(&f->f_refcnt, 1, __ATOMIC_SEQ_CST);
    }
}

void file_close(struct file *f) {
    if (!f) return;

    if (__atomic_fetch_sub(&f->f_refcnt, 1, __ATOMIC_SEQ_CST) == 1) {
        if (f->f_vn) {
            struct vnode *vn = f->f_vn;
            f->f_vn = NULL;
            vput(vn);
        }
        call_rcu(&f->f_rcu, file_free_rcu, f);
    }
}

void file_lock(struct file *f) {
    spin_lock(&f->f_lock);
}

void file_unlock(struct file *f) {
    spin_unlock(&f->f_lock);
}

void file_lock_irqsave(struct file *f, uint64_t *flags) {
    *flags = spin_lock_irqsave(&f->f_lock);
}

void file_unlock_irqrestore(struct file *f, uint64_t flags) {
    spin_unlock_irqrestore(&f->f_lock, flags);
}