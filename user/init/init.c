#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <gbm.h>

// Minimal fb_var_screeninfo to query screen size from /dev/fb0
struct our_fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t pad[33];
};

void run_graphics_tests(void) {
    printf("Opening /dev/dri/card0...\n");
    int card_fd = open("/dev/dri/card0", O_RDWR);
    if (card_fd < 0) {
        perror("Failed to open /dev/dri/card0");
        return;
    }
    printf("Opened /dev/dri/card0 (fd: %d)\n", card_fd);

    printf("Creating GBM device...\n");
    struct gbm_device *gbm = gbm_create_device(card_fd);
    if (!gbm) {
        printf("[Failed to create GBM device\n");
        close(card_fd);
        return;
    }
    printf("GBM device created!\n");

    const char *backend = gbm_device_get_backend_name(gbm);
    printf("GBM Backend Name: %s\n", backend);

    int xrgb_supported = gbm_device_is_format_supported(gbm, GBM_FORMAT_XRGB8888, 0);
    printf("GBM_FORMAT_XRGB8888 support check: %s\n", xrgb_supported ? "SUPPORTED" : "NOT SUPPORTED");

    uint32_t width = 1024;
    uint32_t height = 768;
    int fb_fd = open("/dev/fb0", O_RDONLY);
    if (fb_fd >= 0) {
        struct our_fb_var_screeninfo var = {0};
        if (ioctl(fb_fd, 0x4600, &var) == 0) { // FBIOGET_VSCREENINFO
            width = var.xres;
            height = var.yres;
            printf("Detected screen resolution from /dev/fb0: %dx%d (%d bpp)\n", width, height, var.bits_per_pixel);
        }
        close(fb_fd);
    } else {
        printf("Could not open /dev/fb0, defaulting to %dx%d\n", width, height);
    }

    printf("Creating GBM Buffer Object (%dx%d)...\n", width, height);
    struct gbm_bo *bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);
    if (!bo) {
        printf("Failed to create GBM Buffer Object\n");
        gbm_device_destroy(gbm);
        close(card_fd);
        return;
    }
    printf("GBM Buffer Object created.\n");
    printf("BO properties: Width=%d, Height=%d, Stride=%d, Handle=%d, Offset=0x%x\n",
           gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_stride(bo),
           gbm_bo_get_handle(bo).u32, gbm_bo_get_offset(bo, 0));

    printf("Mapping GBM Buffer Object...\n");
    uint32_t stride = 0;
    void *map_data = NULL;
    uint32_t *pixels = gbm_bo_map(bo, 0, 0, width, height, GBM_BO_USE_WRITE, &stride, &map_data);
    if (!pixels) {
        printf("Failed to map GBM Buffer Object\n");
        gbm_bo_destroy(bo);
        gbm_device_destroy(gbm);
        close(card_fd);
        return;
    }
    printf("Object mapped at %p!\n", pixels);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = (x * 255) / width;
            uint8_t g = (y * 255) / height;
            uint8_t b = ((x + y) * 255) / (width + height);
            
            // XRGB8888
            pixels[y * (stride / 4) + x] = (r << 16) | (g << 8) | b;
        }
    }

    for (int i = 10; i > 0; i--) {
        printf("Returning in %d seconds...\n", i);
        sleep(1);
    }

    gbm_bo_unmap(bo, map_data);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    close(card_fd);
    printf("Cleaned up!\n");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int fd_stdin = open("/dev/tty", O_RDONLY);
    int fd_stdout = open("/dev/tty", O_WRONLY);
    int fd_stderr = open("/dev/tty", O_WRONLY);

    (void)fd_stdin;
    (void)fd_stdout;
    (void)fd_stderr;

    printf("Hello MUSL!\n");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    mkdir("/mnt/usr", 0755);
    mount("/usr", "/mnt/usr", "bind", 0, NULL);

    run_graphics_tests();

    printf("[INIT] END!\n");
    while (1) {
        sleep(3600);
    }

    return 0;
}