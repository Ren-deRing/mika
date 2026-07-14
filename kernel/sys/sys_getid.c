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
#include <kernel/fs/vfs.h>
#include <kernel/fs/vnode.h>
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
    if (!curproc) return 0;
    return curproc->p_uid;
}

int64_t sys_getgid(void) {
    if (!curproc) return 0;
    return curproc->p_gid;
}

int64_t sys_geteuid(void) {
    if (!curproc) return 0;
    return curproc->p_euid;
}

int64_t sys_getegid(void) {
    if (!curproc) return 0;
    return curproc->p_egid;
}

int64_t sys_umask(mode_t mask) {
    if (!curproc) return 0022;
    mode_t old = curproc->p_umask;
    curproc->p_umask = mask & 0777;
    return old;
}

static inline int uid_may_set(uid_t val, uid_t ruid, uid_t euid, uid_t suid) {
    return val == ruid || val == euid || val == suid;
}
static inline int gid_may_set(gid_t val, gid_t rgid, gid_t egid, gid_t sgid) {
    return val == rgid || val == egid || val == sgid;
}

int64_t sys_setuid(uid_t uid) {
    if (!curproc) return -EPERM;
    if (curproc->p_euid == 0) {
        curproc->p_uid = uid;
        curproc->p_euid = uid;
        curproc->p_suid = uid;
        return 0;
    }
    if (uid == curproc->p_uid || uid == curproc->p_suid) {
        curproc->p_euid = uid;
        return 0;
    }
    return -EPERM;
}

int64_t sys_setgid(gid_t gid) {
    if (!curproc) return -EPERM;
    if (curproc->p_euid == 0) {
        curproc->p_gid = gid;
        curproc->p_egid = gid;
        curproc->p_sgid = gid;
        return 0;
    }
    if (gid == curproc->p_gid || gid == curproc->p_sgid) {
        curproc->p_egid = gid;
        return 0;
    }
    return -EPERM;
}

int64_t sys_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    if (!curproc) return -EPERM;
    int privileged = (curproc->p_euid == 0);
    if (!privileged) {
        if (ruid != (uid_t)-1 && !uid_may_set(ruid, curproc->p_uid, curproc->p_euid, curproc->p_suid))
            return -EPERM;
        if (euid != (uid_t)-1 && !uid_may_set(euid, curproc->p_uid, curproc->p_euid, curproc->p_suid))
            return -EPERM;
        if (suid != (uid_t)-1 && !uid_may_set(suid, curproc->p_uid, curproc->p_euid, curproc->p_suid))
            return -EPERM;
    }
    if (ruid != (uid_t)-1) curproc->p_uid = ruid;
    if (euid != (uid_t)-1) curproc->p_euid = euid;
    if (suid != (uid_t)-1) curproc->p_suid = suid;
    return 0;
}

int64_t sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
    if (!curproc) return -EPERM;
    int privileged = (curproc->p_euid == 0);
    if (!privileged) {
        if (rgid != (gid_t)-1 && !gid_may_set(rgid, curproc->p_gid, curproc->p_egid, curproc->p_sgid))
            return -EPERM;
        if (egid != (gid_t)-1 && !gid_may_set(egid, curproc->p_gid, curproc->p_egid, curproc->p_sgid))
            return -EPERM;
        if (sgid != (gid_t)-1 && !gid_may_set(sgid, curproc->p_gid, curproc->p_egid, curproc->p_sgid))
            return -EPERM;
    }
    if (rgid != (gid_t)-1) curproc->p_gid = rgid;
    if (egid != (gid_t)-1) curproc->p_egid = egid;
    if (sgid != (gid_t)-1) curproc->p_sgid = sgid;
    return 0;
}

int64_t sys_setreuid(uid_t ruid, uid_t euid) {
    return sys_setresuid(ruid, euid, (uid_t)-1);
}

int64_t sys_setregid(gid_t rgid, gid_t egid) {
    return sys_setresgid(rgid, egid, (gid_t)-1);
}

static int vfs_chown_internal(const char *path, uid_t uid, gid_t gid) {
    char kpath[256];
    if (copy_str_from_user(kpath, path, 256) < 0) return -EFAULT;

    char clean_path[256];
    sanitize_path(kpath, clean_path, sizeof(clean_path));

    struct vnode *vn = NULL;
    int err = vfs_lookup(clean_path, curproc->p_cwd, &vn);
    if (err < 0) return err;

    if (curproc->p_euid != 0) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (vn->ops && vn->ops->getattr) {
            vn->ops->getattr(vn, &st);
        }
        if (uid != (uid_t)-1 && st.st_uid != curproc->p_euid) {
            vput(vn);
            return -EPERM;
        }
        if (gid != (gid_t)-1 && gid != curproc->p_egid && curproc->p_euid != st.st_uid) {
            vput(vn);
            return -EPERM;
        }
    }

    if (vn->ops && vn->ops->setattr) {
        struct stat st;
        st.st_size = 0;
        st.st_mode = (mode_t)-1;
        st.st_uid = uid;
        st.st_gid = gid;
        err = vn->ops->setattr(vn, &st);
    }
    vput(vn);
    return err;
}

int64_t sys_lchown(const char *user_path, uid_t owner, gid_t group) {
    return vfs_chown_internal(user_path, owner, group);
}

int64_t sys_chown(const char *user_path, uid_t owner, gid_t group) {
    return vfs_chown_internal(user_path, owner, group);
}

int64_t sys_fchown(int fd, uid_t owner, gid_t group) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    struct vnode *vn = f->f_vn;

    if (curproc->p_euid != 0) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (vn->ops && vn->ops->getattr)
            vn->ops->getattr(vn, &st);
        if (owner != (uid_t)-1 && st.st_uid != curproc->p_euid) {
            fdput(f);
            return -EPERM;
        }
        if (group != (gid_t)-1 && group != curproc->p_egid && curproc->p_euid != st.st_uid) {
            fdput(f);
            return -EPERM;
        }
    }

    if (vn->ops && vn->ops->setattr) {
        struct stat st;
        st.st_size = 0;
        st.st_mode = (mode_t)-1;
        st.st_uid = owner;
        st.st_gid = group;
        int err = vn->ops->setattr(vn, &st);
        fdput(f);
        return err;
    }
    fdput(f);
    return -ENOSYS;
}

int64_t sys_chmod(const char *user_path, mode_t mode) {
    char kpath[256];
    if (copy_str_from_user(kpath, user_path, 256) < 0) return -EFAULT;

    char clean_path[256];
    sanitize_path(kpath, clean_path, sizeof(clean_path));

    struct vnode *vn = NULL;
    int err = vfs_lookup(clean_path, curproc->p_cwd, &vn);
    if (err < 0) return err;

    if (vn->ops && vn->ops->getattr) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (vn->ops->getattr(vn, &st) == 0) {
            if (curproc->p_euid != 0 && st.st_uid != curproc->p_euid) {
                vput(vn);
                return -EPERM;
            }
        }
    }

    if (vn->ops && vn->ops->setattr) {
        struct stat st;
        st.st_size = 0;
        st.st_mode = mode & 07777;
        st.st_uid = (uid_t)-1;
        st.st_gid = (gid_t)-1;
        err = vn->ops->setattr(vn, &st);
    }
    vput(vn);
    return err;
}

int64_t sys_fchmod(int fd, mode_t mode) {
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    struct vnode *vn = f->f_vn;

    if (vn->ops && vn->ops->getattr) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (vn->ops->getattr(vn, &st) == 0) {
            if (curproc->p_euid != 0 && st.st_uid != curproc->p_euid) {
                fdput(f);
                return -EPERM;
            }
        }
    }

    if (vn->ops && vn->ops->setattr) {
        struct stat st;
        st.st_size = 0;
        st.st_mode = mode & 07777;
        st.st_uid = (uid_t)-1;
        st.st_gid = (gid_t)-1;
        int err = vn->ops->setattr(vn, &st);
        fdput(f);
        return err;
    }
    fdput(f);
    return -ENOSYS;
}

int64_t sys_setsid(void) {
    if (!curproc) return -EINVAL;
    return curproc->p_pid;
}
