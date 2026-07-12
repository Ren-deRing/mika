#pragma once

#include <stdint.h>

struct vnode;
struct vnode_ops;

#define MAX_CDEV 64

struct cdev {
    int major;
    int minor_start;
    int minor_count;
    struct vnode_ops *ops;
};

int cdev_register(struct cdev *cd);
struct cdev *cdev_get(int major);
