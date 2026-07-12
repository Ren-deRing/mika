#include <kernel/cdev.h>
#include <kernel/lock.h>
#include <kernel/printf.h>

static struct cdev *cdev_table[MAX_CDEV];
static spinlock_t cdev_lock;

int cdev_register(struct cdev *cd) {
    if (!cd || !cd->ops || cd->major < 0 || cd->major >= MAX_CDEV) {
        dprintf("[CDEV] Invalid cdev registration (major=%d)\n", cd ? cd->major : -1);
        return -1;
    }

    spin_lock(&cdev_lock);
    if (cdev_table[cd->major]) {
        spin_unlock(&cdev_lock);
        dprintf("[CDEV] Major %d already registered\n", cd->major);
        return -1;
    }
    cdev_table[cd->major] = cd;
    spin_unlock(&cdev_lock);

    return 0;
}

struct cdev *cdev_get(int major) {
    if (major < 0 || major >= MAX_CDEV) return NULL;
    struct cdev *cd;
    spin_lock(&cdev_lock);
    cd = cdev_table[major];
    spin_unlock(&cdev_lock);
    return cd;
}
