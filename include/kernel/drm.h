#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct file;

bool has_drm_event(void);
int64_t drm_read(struct file *f, void *user_buf, size_t count);
int64_t drm_ioctl(struct file *f, uint32_t request, void *arg);
