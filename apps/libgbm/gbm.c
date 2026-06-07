#include "gbm.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define DRM_IOCTL_MODE_CREATE_DUMB  0xc02064b2
#define DRM_IOCTL_MODE_MAP_DUMB     0xc01064b3

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct gbm_device {
    int fd;
};

struct gbm_bo {
    struct gbm_device *gbm;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    uint64_t offset;
    void *map_address;
    void *user_data;
    void (*destroy_user_data)(struct gbm_bo *, void *);
};

struct gbm_surface {
    struct gbm_device *gbm;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t flags;
    struct gbm_bo *bos[3];
    int num_bos;
    int current_bo;
};


int gbm_device_get_fd(struct gbm_device *gbm) {
    if (!gbm) return -1;
    return gbm->fd;
}

const char *gbm_device_get_backend_name(struct gbm_device *gbm) {
    (void)gbm;
    return "mika_drm";
}

int gbm_device_is_format_supported(struct gbm_device *gbm, uint32_t format, uint32_t flags) {
    (void)gbm;
    (void)flags;
    return format == GBM_FORMAT_XRGB8888 || format == GBM_FORMAT_ARGB8888;
}

void gbm_device_destroy(struct gbm_device *gbm) {
    if (gbm) {
        free(gbm);
    }
}

struct gbm_device *gbm_create_device(int fd) {
    struct gbm_device *gbm = malloc(sizeof(*gbm));
    if (!gbm) return NULL;
    gbm->fd = fd;
    return gbm;
}


uint32_t gbm_bo_get_width(struct gbm_bo *bo) {
    return bo ? bo->width : 0;
}

uint32_t gbm_bo_get_height(struct gbm_bo *bo) {
    return bo ? bo->height : 0;
}

uint32_t gbm_bo_get_stride(struct gbm_bo *bo) {
    return bo ? bo->pitch : 0;
}

uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane) {
    (void)plane;
    return gbm_bo_get_stride(bo);
}

uint32_t gbm_bo_get_format(struct gbm_bo *bo) {
    return bo ? bo->format : 0;
}

struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo) {
    return bo ? bo->gbm : NULL;
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo) {
    union gbm_bo_handle handle = {0};
    if (bo) {
        handle.u32 = bo->handle;
    }
    return handle;
}

union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane) {
    (void)plane;
    return gbm_bo_get_handle(bo);
}

int gbm_bo_get_fd(struct gbm_bo *bo) {
    if (!bo) return -1;
    // dmabufs 없으니까 일단
    return dup(bo->gbm->fd);
}

uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) {
    (void)bo;
    return 0; // DRM_FORMAT_MOD_LINEAR
}

uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane) {
    (void)plane;
    return bo ? (uint32_t)bo->offset : 0;
}

struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width, uint32_t height,
                             uint32_t format, uint32_t flags) {
    if (!gbm) return NULL;

    struct drm_mode_create_dumb db = {0};
    db.width = width;
    db.height = height;
    db.bpp = 32;
    db.flags = flags;

    if (ioctl(gbm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &db) < 0) {
        return NULL;
    }

    struct drm_mode_map_dumb md = {0};
    md.handle = db.handle;
    if (ioctl(gbm->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        return NULL;
    }

    struct gbm_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) return NULL;

    bo->gbm = gbm;
    bo->width = width;
    bo->height = height;
    bo->format = format;
    bo->flags = flags;
    bo->handle = db.handle;
    bo->pitch = db.pitch;
    bo->size = db.size;
    bo->offset = md.offset;
    bo->map_address = NULL;

    return bo;
}

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                            uint32_t format, const uint64_t *modifiers, const unsigned int count) {
    (void)modifiers;
    (void)count;
    return gbm_bo_create(gbm, width, height, format, 0);
}

void gbm_bo_destroy(struct gbm_bo *bo) {
    if (!bo) return;

    if (bo->destroy_user_data && bo->user_data) {
        bo->destroy_user_data(bo, bo->user_data);
    }

    if (bo->map_address) {
        munmap(bo->map_address, bo->size);
    }

    // 아직은 뭐 그냥 free해도 괜찮은듯?
    free(bo);
}

void gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
                          void (*destroy_user_data)(struct gbm_bo *, void *)) {
    if (!bo) return;
    bo->user_data = data;
    bo->destroy_user_data = destroy_user_data;
}

void *gbm_bo_get_user_data(struct gbm_bo *bo) {
    return bo ? bo->user_data : NULL;
}

/* Buffer Mapping Functions */

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                 uint32_t flags, uint32_t *stride, void **map_data) {
    (void)flags;
    (void)width;
    (void)height;

    if (!bo) return NULL;

    if (!bo->map_address) {
        bo->map_address = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->gbm->fd, bo->offset);
        if (bo->map_address == MAP_FAILED) {
            bo->map_address = NULL;
            return NULL;
        }
    }

    *stride = bo->pitch;
    *map_data = bo->map_address;

    return (void *)((uintptr_t)bo->map_address + y * bo->pitch + x * 4);
}

void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) {
    (void)bo;
    (void)map_data;
}


struct gbm_surface *gbm_surface_create(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                       uint32_t format, uint32_t flags) {
    struct gbm_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) return NULL;

    surface->gbm = gbm;
    surface->width = width;
    surface->height = height;
    surface->format = format;
    surface->flags = flags;
    surface->num_bos = 3;
    surface->current_bo = 0;

    return surface;
}

struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                                      uint32_t format, const uint64_t *modifiers, const unsigned int count) {
    (void)modifiers;
    (void)count;
    return gbm_surface_create(gbm, width, height, format, 0);
}

void gbm_surface_destroy(struct gbm_surface *surface) {
    if (!surface) return;

    for (int i = 0; i < 3; i++) {
        if (surface->bos[i]) {
            gbm_bo_destroy(surface->bos[i]);
        }
    }
    free(surface);
}

struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface) {
    if (!surface) return NULL;

    int idx = surface->current_bo;
    if (!surface->bos[idx]) {
        surface->bos[idx] = gbm_bo_create(surface->gbm, surface->width, surface->height,
                                          surface->format, surface->flags);
    }

    struct gbm_bo *bo = surface->bos[idx];
    surface->current_bo = (idx + 1) % surface->num_bos;
    return bo;
}

void gbm_surface_release_buffer(struct gbm_surface *surface, struct gbm_bo *bo) {
    (void)surface;
    (void)bo;
}

int gbm_surface_has_free_buffers(struct gbm_surface *surface) {
    (void)surface;
    return 1;
}
