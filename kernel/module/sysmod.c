#include <kernel/module.h>
#include <kernel/proc.h>
#include <kernel/printf.h>
#include <kernel/fs/vfs.h>
#include <kernel/syscall.h>
#include <uapi/errno.h>
#include <string.h>

int64_t sys_finit_module(int fd, const char *user_params, int flags) {
    (void)flags;

    if (user_params) {
        char kern_params[256];
        if (copy_from_user(kern_params, user_params, sizeof(kern_params)) < 0)
            return -EFAULT;
        if (kern_params[0] != '\0')
            dprintf("[module] params: %s\n", kern_params);
    }

    struct module *mod = module_load_from_fd(fd);
    if (!mod) return -ENOMEM;

    int ret = module_load(mod);
    return ret;
}

int64_t sys_delete_module(const char *user_name, int flags) {
    (void)flags;

    char name[MODULE_NAME_LEN];
    if (copy_from_user(name, user_name, MODULE_NAME_LEN) < 0)
        return -EFAULT;
    name[MODULE_NAME_LEN - 1] = '\0';

    return module_unload(name);
}
