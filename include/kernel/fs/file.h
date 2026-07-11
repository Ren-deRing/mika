#pragma once

#include <kernel/lock_types.h>
#include <kernel/fs/vnode.h>
#include <kernel/rcu.h>
#include <uapi/types.h>

struct file {
    struct vnode *f_vn;
    uint32_t      f_flags;    /* 열기 모드 - f_lock */
    off_t         f_pos;      /* 현재 위치 - f_lock */
    uint32_t      f_refcnt;   /* atomic */
    struct rcu_head f_rcu;    /* deferred free */
};

struct file *file_alloc(void);
void file_ref(struct file *f);
void file_close(struct file *f);


struct file *fdget(int fd);
void fdput(struct file *f);