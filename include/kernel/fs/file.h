#pragma once

#include <kernel/lock_types.h>
#include <kernel/fs/vnode.h>
#include <uapi/types.h>

struct file {
    struct vnode *f_vn;       /* vnode - f_lock */
    uint32_t      f_flags;    /* 열기 모드 - f_lock */
    off_t         f_pos;      /* 현재 위치 - f_lock */
    uint32_t      f_refcnt;   /* atomic */
    spinlock_t    f_lock;
};

struct file *file_alloc(void);
void file_ref(struct file *f);
void file_close(struct file *f);
void file_lock(struct file *f);
void file_unlock(struct file *f);
void file_lock_irqsave(struct file *f, uint64_t *flags);
void file_unlock_irqrestore(struct file *f, uint64_t flags);