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

