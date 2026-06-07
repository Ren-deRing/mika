#include <uapi/errno.h>
#include <boot/bootinfo.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <kernel/lock.h>
#include <kernel/clock.h>
#include <kernel/fs/file.h>
#include <kernel/fs/vnode.h>
#include <kernel/fs/vfs.h>
#include <string.h>
#include <kernel/drm.h>

#define DRM_IOCTL_VERSION           0xc0406400
#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0
#define DRM_IOCTL_MODE_GETCRTC      0xc06864a1
#define DRM_IOCTL_MODE_SETCRTC      0xc06864a2
#define DRM_IOCTL_MODE_GETCONNECTOR 0xc05064a7
#define DRM_IOCTL_MODE_CREATE_DUMB  0xc02064b2
#define DRM_IOCTL_MODE_MAP_DUMB     0xc01064b3
#define DRM_IOCTL_MODE_GETPROPERTY  0xc04064aa
#define DRM_IOCTL_MODE_GETPLANE     0xc02064b6
#define DRM_IOCTL_MODE_ADDFB2       0xc01c64ae
#define DRM_IOCTL_MODE_GETFB        0xc06864b8
#define DRM_IOCTL_MODE_PAGE_FLIP    0xc01864b0

#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

struct drm_event {
    uint32_t type;
    uint32_t length;
};

struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

struct drm_mode_page_flip {
    uint32_t fb_id;
    uint32_t crtc_id;
    uint64_t user_data;
    uint32_t flags;
    uint32_t reserved;
};

#define MAX_DRM_EVENTS 16
static struct drm_event_vblank g_drm_events[MAX_DRM_EVENTS];
static int g_drm_event_head = 0;
static int g_drm_event_tail = 0;
static spinlock_t g_drm_events_lock = SPINLOCK_INITIALIZER;

static void queue_drm_event(struct drm_event_vblank *ev) {
    uint64_t flags = spin_lock_irqsave(&g_drm_events_lock);
    int next_tail = (g_drm_event_tail + 1) % MAX_DRM_EVENTS;
    if (next_tail != g_drm_event_head) {
        g_drm_events[g_drm_event_tail] = *ev;
        g_drm_event_tail = next_tail;
    }
    spin_unlock_irqrestore(&g_drm_events_lock, flags);
}

bool has_drm_event(void) {
    uint64_t flags = spin_lock_irqsave(&g_drm_events_lock);
    bool available = (g_drm_event_head != g_drm_event_tail);
    spin_unlock_irqrestore(&g_drm_events_lock, flags);
    return available;
}

static int dequeue_drm_event(struct drm_event_vblank *ev) {
    uint64_t flags = spin_lock_irqsave(&g_drm_events_lock);
    if (g_drm_event_head == g_drm_event_tail) {
        spin_unlock_irqrestore(&g_drm_events_lock, flags);
        return 0;
    }
    *ev = g_drm_events[g_drm_event_head];
    g_drm_event_head = (g_drm_event_head + 1) % MAX_DRM_EVENTS;
    spin_unlock_irqrestore(&g_drm_events_lock, flags);
    return 1;
}

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    uint64_t name_len;
    uint64_t name;
    uint64_t date_len;
    uint64_t date;
    uint64_t desc_len;
    uint64_t desc;
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_get_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

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

struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};

struct drm_mode_property_enum {
    uint64_t value;
    char name[32];
};

int64_t drm_read(struct file *f, void *user_buf, size_t count) {
    (void)f;
    struct drm_event_vblank ev;
    if (!dequeue_drm_event(&ev)) {
        return -EAGAIN;
    }
    size_t to_copy = (count < sizeof(struct drm_event_vblank)) ? count : sizeof(struct drm_event_vblank);
    if (copy_to_user(user_buf, &ev, to_copy) < 0) {
        return -EFAULT;
    }
    return (int64_t)to_copy;
}

int64_t drm_ioctl(struct file *f, uint32_t request, void *arg) {
    (void)f;
    if (request == DRM_IOCTL_VERSION) {
        struct drm_version ver = {0};
        char *name = "mika_drm";
        char *date = "20260607";
        char *desc = "Mika DRM/KMS Dumb Driver";

        if (!is_user_address_range(arg, sizeof(struct drm_version))) return -EFAULT;
        if (copy_from_user(&ver, arg, sizeof(struct drm_version)) < 0) return -EFAULT;

        ver.version_major = 1;
        ver.version_minor = 0;
        ver.version_patchlevel = 0;

        if (ver.name && ver.name_len > 0) {
            size_t to_copy = (strlen(name) < ver.name_len) ? strlen(name) : ver.name_len;
            if (copy_to_user((void*)ver.name, name, to_copy) < 0) return -EFAULT;
            ver.name_len = to_copy;
        }
        if (ver.date && ver.date_len > 0) {
            size_t to_copy = (strlen(date) < ver.date_len) ? strlen(date) : ver.date_len;
            if (copy_to_user((void*)ver.date, date, to_copy) < 0) return -EFAULT;
            ver.date_len = to_copy;
        }
        if (ver.desc && ver.desc_len > 0) {
            size_t to_copy = (strlen(desc) < ver.desc_len) ? strlen(desc) : ver.desc_len;
            if (copy_to_user((void*)ver.desc, desc, to_copy) < 0) return -EFAULT;
            ver.desc_len = to_copy;
        }

        if (copy_to_user(arg, &ver, sizeof(struct drm_version)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == 0xc010640c) { // DRM_IOCTL_GET_CAP
        struct {
            uint64_t capability;
            uint64_t value;
        } cap = {0};
        if (!is_user_address_range(arg, sizeof(cap))) return -EFAULT;
        if (copy_from_user(&cap, arg, sizeof(cap)) < 0) return -EFAULT;

        if (cap.capability == 0x6) { // DRM_CAP_TIMESTAMP_MONOTONIC
            cap.value = 1;
        } else if (cap.capability == 0x1) { // DRM_CAP_DUMB_BUFFER
            cap.value = 1;
        } else if (cap.capability == 0x8) { // DRM_CAP_CURSOR_WIDTH
            cap.value = 64;
        } else if (cap.capability == 0x9) { // DRM_CAP_CURSOR_HEIGHT
            cap.value = 64;
        } else {
            cap.value = 0;
        }

        if (copy_to_user(arg, &cap, sizeof(cap)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == 0x4010640d) { // DRM_IOCTL_SET_CLIENT_CAP
        struct {
            uint64_t capability;
            uint64_t value;
        } cap = {0};
        if (!is_user_address_range(arg, sizeof(cap))) return -EFAULT;
        if (copy_from_user(&cap, arg, sizeof(cap)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_GETRESOURCES) {
        struct drm_mode_card_res res = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_card_res))) return -EFAULT;
        if (copy_from_user(&res, arg, sizeof(struct drm_mode_card_res)) < 0) return -EFAULT;

        res.count_fbs = 1;
        res.count_crtcs = 1;
        res.count_connectors = 1;
        res.count_encoders = 1;
        res.min_width = 320;
        res.max_width = 8192;
        res.min_height = 240;
        res.max_height = 8192;

        uint32_t fb_id = 1;
        uint32_t crtc_id = 2;
        uint32_t connector_id = 3;
        uint32_t encoder_id = 4;

        if (res.fb_id_ptr && res.count_fbs >= 1) {
            if (!is_user_address_range((void*)res.fb_id_ptr, sizeof(uint32_t))) return -EFAULT;
            if (copy_to_user((void*)res.fb_id_ptr, &fb_id, sizeof(uint32_t)) < 0) return -EFAULT;
        }
        if (res.crtc_id_ptr && res.count_crtcs >= 1) {
            if (!is_user_address_range((void*)res.crtc_id_ptr, sizeof(uint32_t))) return -EFAULT;
            if (copy_to_user((void*)res.crtc_id_ptr, &crtc_id, sizeof(uint32_t)) < 0) return -EFAULT;
        }
        if (res.connector_id_ptr && res.count_connectors >= 1) {
            if (!is_user_address_range((void*)res.connector_id_ptr, sizeof(uint32_t))) return -EFAULT;
            if (copy_to_user((void*)res.connector_id_ptr, &connector_id, sizeof(uint32_t)) < 0) return -EFAULT;
        }
        if (res.encoder_id_ptr && res.count_encoders >= 1) {
            if (!is_user_address_range((void*)res.encoder_id_ptr, sizeof(uint32_t))) return -EFAULT;
            if (copy_to_user((void*)res.encoder_id_ptr, &encoder_id, sizeof(uint32_t)) < 0) return -EFAULT;
        }

        if (copy_to_user(arg, &res, sizeof(struct drm_mode_card_res)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_GETCONNECTOR) {
        struct drm_mode_get_connector conn = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_get_connector))) return -EFAULT;
        if (copy_from_user(&conn, arg, sizeof(struct drm_mode_get_connector)) < 0) return -EFAULT;

        conn.connector_id = 3;
        conn.connector_type = 11; // HDMI-A
        conn.connector_type_id = 1;
        conn.connection = 1; // Connected
        conn.mm_width = 294;
        conn.mm_height = 165;
        conn.subpixel = 1;
        conn.encoder_id = 4;

        conn.count_modes = 1;
        conn.count_props = 0;
        conn.count_encoders = 1;

        uint32_t enc_id = 4;
        if (conn.encoders_ptr && conn.count_encoders >= 1) {
            if (!is_user_address_range((void*)conn.encoders_ptr, sizeof(uint32_t))) return -EFAULT;
            if (copy_to_user((void*)conn.encoders_ptr, &enc_id, sizeof(uint32_t)) < 0) return -EFAULT;
        }

        if (conn.modes_ptr && conn.count_modes >= 1) {
            if (!is_user_address_range((void*)conn.modes_ptr, sizeof(struct drm_mode_modeinfo))) return -EFAULT;
            struct drm_mode_modeinfo mode = {0};
            mode.clock = 74250;
            mode.hdisplay = g_boot_info.fb.width;
            mode.hsync_start = g_boot_info.fb.width + 40;
            mode.hsync_end = g_boot_info.fb.width + 80;
            mode.htotal = g_boot_info.fb.width + 120;
            mode.vdisplay = g_boot_info.fb.height;
            mode.vsync_start = g_boot_info.fb.height + 10;
            mode.vsync_end = g_boot_info.fb.height + 20;
            mode.vtotal = g_boot_info.fb.height + 30;
            mode.vrefresh = 60;
            mode.flags = 0;
            mode.type = 72; // DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER
            strcpy(mode.name, "Doppio Mode");
            if (copy_to_user((void*)conn.modes_ptr, &mode, sizeof(struct drm_mode_modeinfo)) < 0) return -EFAULT;
        }

        if (copy_to_user(arg, &conn, sizeof(struct drm_mode_get_connector)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_GETCRTC) {
        struct drm_mode_get_crtc crtc = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_get_crtc))) return -EFAULT;
        if (copy_from_user(&crtc, arg, sizeof(struct drm_mode_get_crtc)) < 0) return -EFAULT;

        crtc.crtc_id = 2;
        crtc.fb_id = 1;
        crtc.x = 0;
        crtc.y = 0;
        crtc.gamma_size = 0;
        crtc.mode_valid = 1;

        crtc.mode.clock = 74250;
        crtc.mode.hdisplay = g_boot_info.fb.width;
        crtc.mode.hsync_start = g_boot_info.fb.width + 40;
        crtc.mode.hsync_end = g_boot_info.fb.width + 80;
        crtc.mode.htotal = g_boot_info.fb.width + 120;
        crtc.mode.vdisplay = g_boot_info.fb.height;
        crtc.mode.vsync_start = g_boot_info.fb.height + 10;
        crtc.mode.vsync_end = g_boot_info.fb.height + 20;
        crtc.mode.vtotal = g_boot_info.fb.height + 30;
        crtc.mode.vrefresh = 60;
        crtc.mode.flags = 0;
        crtc.mode.type = 72;
        strcpy(crtc.mode.name, "Doppio Mode");

        if (copy_to_user(arg, &crtc, sizeof(struct drm_mode_get_crtc)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
        struct drm_mode_create_dumb db = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_create_dumb))) return -EFAULT;
        if (copy_from_user(&db, arg, sizeof(struct drm_mode_create_dumb)) < 0) return -EFAULT;

        db.pitch = db.width * (db.bpp / 8);
        db.size = (uint64_t)db.pitch * db.height;
        
        static uint32_t g_next_dumb_handle = 1;
        db.handle = g_next_dumb_handle++;

        if (copy_to_user(arg, &db, sizeof(struct drm_mode_create_dumb)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_MAP_DUMB) {
        struct drm_mode_map_dumb md = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_map_dumb))) return -EFAULT;
        if (copy_from_user(&md, arg, sizeof(struct drm_mode_map_dumb)) < 0) return -EFAULT;

        md.offset = 0x10000000 * md.handle;

        if (copy_to_user(arg, &md, sizeof(struct drm_mode_map_dumb)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == 0xc02064b9) { // DRM_IOCTL_MODE_OBJ_GETPROPERTIES
        struct {
            uint64_t props_ptr;
            uint64_t prop_values_ptr;
            uint32_t count_props;
            uint32_t obj_id;
            uint32_t obj_type;
            uint32_t pad;
        } obj_props = {0};
        if (!is_user_address_range(arg, sizeof(obj_props))) return -EFAULT;
        if (copy_from_user(&obj_props, arg, sizeof(obj_props)) < 0) return -EFAULT;

        if (obj_props.obj_id == 5) { // Plane 5
            uint32_t prop_id = 100; // Property ID for "type"
            uint64_t prop_val = 1;  // DRM_PLANE_TYPE_PRIMARY = 1

            if (obj_props.props_ptr && obj_props.count_props >= 1) {
                if (!is_user_address_range((void*)obj_props.props_ptr, sizeof(uint32_t))) return -EFAULT;
                if (copy_to_user((void*)obj_props.props_ptr, &prop_id, sizeof(uint32_t)) < 0) return -EFAULT;
            }
            if (obj_props.prop_values_ptr && obj_props.count_props >= 1) {
                if (!is_user_address_range((void*)obj_props.prop_values_ptr, sizeof(uint64_t))) return -EFAULT;
                if (copy_to_user((void*)obj_props.prop_values_ptr, &prop_val, sizeof(uint64_t)) < 0) return -EFAULT;
            }
            obj_props.count_props = 1;
        } else {
            obj_props.count_props = 0;
        }

        if (copy_to_user(arg, &obj_props, sizeof(obj_props)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == 0xc01064b5) { // DRM_IOCTL_MODE_GETPLANES
        struct {
            uint64_t plane_id_ptr;
            uint32_t count_planes;
            uint32_t pad;
        } planes = {0};
        if (!is_user_address_range(arg, sizeof(planes))) return -EFAULT;
        if (copy_from_user(&planes, arg, sizeof(planes)) < 0) return -EFAULT;

        uint32_t plane_id = 5;
        if (planes.plane_id_ptr && planes.count_planes >= 1) {
            if (!is_user_address_range((void*)planes.plane_id_ptr, sizeof(uint32_t))) return -EFAULT;
            if (copy_to_user((void*)planes.plane_id_ptr, &plane_id, sizeof(uint32_t)) < 0) return -EFAULT;
        }
        planes.count_planes = 1;

        if (copy_to_user(arg, &planes, sizeof(planes)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_GETPLANE) {
        struct drm_mode_get_plane plane = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_get_plane))) return -EFAULT;
        if (copy_from_user(&plane, arg, sizeof(struct drm_mode_get_plane)) < 0) return -EFAULT;

        plane.plane_id = 5;
        plane.crtc_id = 2;
        plane.fb_id = 1;
        plane.possible_crtcs = 1;
        plane.gamma_size = 0;
        plane.count_format_types = 1;

        if (plane.format_type_ptr && plane.count_format_types >= 1) {
            if (!is_user_address_range((void*)plane.format_type_ptr, sizeof(uint32_t))) return -EFAULT;
            uint32_t format = 0x34325258; // DRM_FORMAT_XRGB8888
            if (copy_to_user((void*)plane.format_type_ptr, &format, sizeof(uint32_t)) < 0) return -EFAULT;
        }

        if (copy_to_user(arg, &plane, sizeof(struct drm_mode_get_plane)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_GETPROPERTY) {
        struct drm_mode_get_property prop = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_get_property))) return -EFAULT;
        if (copy_from_user(&prop, arg, sizeof(struct drm_mode_get_property)) < 0) return -EFAULT;

        if (prop.prop_id == 100) {
            prop.flags = 8; // DRM_MODE_PROP_ENUM
            strcpy(prop.name, "type");
            prop.count_values = 0;

            uint32_t orig_count = prop.count_enum_blobs;
            prop.count_enum_blobs = 3;

            if (prop.enum_blob_ptr && orig_count > 0) {
                uint32_t copy_count = orig_count < 3 ? orig_count : 3;
                if (!is_user_address_range((void*)prop.enum_blob_ptr, sizeof(struct drm_mode_property_enum) * copy_count)) return -EFAULT;

                struct drm_mode_property_enum enums[3];
                enums[0].value = 1;
                strcpy(enums[0].name, "Primary");
                enums[1].value = 0;
                strcpy(enums[1].name, "Overlay");
                enums[2].value = 2;
                strcpy(enums[2].name, "Cursor");

                if (copy_to_user((void*)prop.enum_blob_ptr, enums, sizeof(struct drm_mode_property_enum) * copy_count) < 0) return -EFAULT;
            }
        }

        if (copy_to_user(arg, &prop, sizeof(struct drm_mode_get_property)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_ADDFB2) {
        struct drm_mode_fb_cmd cmd = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_fb_cmd))) return -EFAULT;
        if (copy_from_user(&cmd, arg, sizeof(struct drm_mode_fb_cmd)) < 0) return -EFAULT;

        static uint32_t g_next_fb_id = 11;
        cmd.fb_id = g_next_fb_id++;

        if (copy_to_user(arg, &cmd, sizeof(struct drm_mode_fb_cmd)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_GETFB) {
        struct drm_mode_fb_cmd2 cmd = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_fb_cmd2))) return -EFAULT;
        if (copy_from_user(&cmd, arg, sizeof(struct drm_mode_fb_cmd2)) < 0) return -EFAULT;

        cmd.fb_id = 1;
        cmd.width = g_boot_info.fb.width;
        cmd.height = g_boot_info.fb.height;
        cmd.pitches[0] = g_boot_info.fb.pitch;
        cmd.pixel_format = 0x34325258; // DRM_FORMAT_XRGB8888
        cmd.handles[0] = 1;

        if (copy_to_user(arg, &cmd, sizeof(struct drm_mode_fb_cmd2)) < 0) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_SETCRTC) {
        if (!is_user_address_range(arg, 104)) return -EFAULT;
        return 0;
    }
    else if (request == DRM_IOCTL_MODE_PAGE_FLIP) {
        struct drm_mode_page_flip flip = {0};
        if (!is_user_address_range(arg, sizeof(struct drm_mode_page_flip))) return -EFAULT;
        if (copy_from_user(&flip, arg, sizeof(struct drm_mode_page_flip)) < 0) return -EFAULT;

        if (flip.flags & DRM_MODE_PAGE_FLIP_EVENT) {
            struct drm_event_vblank ev = {0};
            ev.base.type = DRM_EVENT_FLIP_COMPLETE;
            ev.base.length = sizeof(struct drm_event_vblank);
            ev.user_data = flip.user_data;

            static uint32_t seq = 0;
            ev.sequence = ++seq;

            uint64_t uptime_ns = get_uptime_ns();
            ev.tv_sec = uptime_ns / 1000000000ULL;
            ev.tv_usec = (uptime_ns % 1000000000ULL) / 1000;
            ev.crtc_id = flip.crtc_id;

            queue_drm_event(&ev);
        }
        return 0;
    }
    return -EINVAL;
}
