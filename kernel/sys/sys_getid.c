#include <uapi/errno.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/vma.h>
#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/kmem.h>
#include <kernel/list.h>
#include <kernel/syscall.h>
#include <kernel/version.h>
#include <string.h>

int64_t sys_getpid(void) {
    if (curproc && curproc->p_pid > 0) {
        return curproc->p_pid;
    }
    return 1;
}

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int64_t sys_uname(void *user_buf) {
    if (!is_user_address_range(user_buf, sizeof(struct utsname))) {
        return -EFAULT;
    }

    struct utsname kbuf;
    memset(&kbuf, 0, sizeof(struct utsname));
    strncpy(kbuf.sysname, __kernel_name, 64);
    strncpy(kbuf.nodename, "mika-qemu", 64);
    strncpy(kbuf.release, __kernel_release, 64);
    strncpy(kbuf.version, __kernel_version, 64);
    strncpy(kbuf.machine, __kernel_machine, 64);
    strncpy(kbuf.domainname, "mika.local", 64);

    if (copy_to_user(user_buf, &kbuf, sizeof(struct utsname)) < 0) {
        return -EFAULT;
    }

    return 0;
}

int64_t sys_getuid(void) {
    return 0;
}

int64_t sys_getgid(void) {
    return 0;
}

int64_t sys_geteuid(void) {
    return 0;
}

int64_t sys_getegid(void) {
    return 0;
}

int64_t sys_lchown(const char *user_path, uid_t owner, gid_t group) {
    (void)user_path;
    (void)owner;
    (void)group;
    return 0;
}

int64_t sys_setsid(void) {
    if (!curproc) return -EINVAL;
    return curproc->p_pid;
}

int64_t sys_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    (void)ruid;
    (void)euid;
    (void)suid;
    return 0;
}
