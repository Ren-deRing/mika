#include <kernel/printf.h>
#include <kernel/kmem.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/file.h>
#include <kernel/fs/vnode.h>
#include <kernel/fd.h>
#include <kernel/socket.h>

#include <uapi/errno.h>
#include <uapi/fcntl.h>

#include <string.h>

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SCM_RIGHTS 1

#define MSG_DONTWAIT 0x40

#define MAX_BOUND_SOCKETS 64

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;
    uint32_t msg_namelen;
    struct iovec *msg_iov;
    uint64_t msg_iovlen;
    void *msg_control;
    uint64_t msg_controllen;
    int32_t msg_flags;
};

struct cmsghdr {
    uint64_t cmsg_len;
    int32_t cmsg_level;
    int32_t cmsg_type;
};


#define AF_NETLINK 16
#define NETLINK_KOBJECT_UEVENT 15

struct sockaddr_nl {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

struct netlink_socket {
    spinlock_t lock;
    int bound_group;
    bool nonblock;
};

static ssize_t netlink_sock_read(struct vnode *vp, void *buf, size_t n, off_t off) {
    (void)vp; (void)buf; (void)n; (void)off;
    return -EAGAIN;
}

static ssize_t netlink_sock_write(struct vnode *vp, const void *buf, size_t n, off_t off) {
    (void)vp; (void)buf; (void)n; (void)off;
    return -EINVAL;
}

static int netlink_sock_close(struct vnode *vp) {
    if (vp->data) {
        kfree(vp->data);
        vp->data = NULL;
    }
    return 0;
}

struct vnode_ops netlink_socket_ops = {
    .read = netlink_sock_read,
    .write = netlink_sock_write,
    .inactive = netlink_sock_close,
};

int64_t sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen) {
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = curproc->p_fd_table[fd];
    if (!f || !f->f_vn) return -ENOTSOCK;
    return 0;
}

static struct unix_socket *bound_sockets[MAX_BOUND_SOCKETS];
static spinlock_t bound_sockets_lock;
static bool bound_sockets_lock_initialized = false;

static void ensure_global_lock_init(void) {
    if (!bound_sockets_lock_initialized) {
        spin_lock_init(&bound_sockets_lock);
        bound_sockets_lock_initialized = true;
    }
}


static ssize_t sock_read(struct vnode *vp, void *buf, size_t n, off_t off);
static ssize_t sock_read_internal(struct vnode *vp, void *buf, size_t n, bool nonblock);
static ssize_t sock_write(struct vnode *vp, const void *buf, size_t n, off_t off);
static int sock_close(struct vnode *vp);

struct vnode_ops unix_socket_ops = {
    .read = sock_read,
    .write = sock_write,
    .inactive = sock_close,
};


static struct unix_socket *sock_alloc(void) {
    struct unix_socket *s = kmalloc(sizeof(struct unix_socket));
    if (!s) return NULL;
    memset(s, 0, sizeof(struct unix_socket));
    spin_lock_init(&s->lock);

    s->buf = kmalloc(UNIX_SOCKET_BUF_SIZE);
    if (!s->buf) {
        kfree(s);
        return NULL;
    }
    memset(s->buf, 0, UNIX_SOCKET_BUF_SIZE);
    s->state = SS_CLOSED;
    return s;
}

static void sock_free(struct unix_socket *s) {
    if (!s) return;
    if (s->buf) kfree(s->buf);
    
    spin_lock(&s->lock);
    
    if (s->peer) {
        spin_lock(&s->peer->lock);
        s->peer->state = SS_DISCONNECTED;
        spin_unlock(&s->peer->lock);
    }
    
    while (s->passed_head != s->passed_tail) {
        struct file *f = s->passed_files[s->passed_head];
        if (f) file_close(f);
        s->passed_head = (s->passed_head + 1) % UNIX_SOCKET_MAX_PASSED_FDS;
    }
    spin_unlock(&s->lock);
    
    kfree(s);
}


static ssize_t sock_read_internal(struct vnode *vp, void *buf, size_t n, bool nonblock) {
    struct unix_socket *s = (struct unix_socket *)vp->data;
    if (!s) return -EINVAL;

    spin_lock(&s->lock);
    if (s->state != SS_CONNECTED && s->buf_head == s->buf_tail) {
        spin_unlock(&s->lock);
        return 0; 
    }

    if (s->buf_head == s->buf_tail) {
        if (nonblock) {
            spin_unlock(&s->lock);
            return -EAGAIN;
        }
        
        while (s->buf_head == s->buf_tail && s->state == SS_CONNECTED) {
            spin_unlock(&s->lock);
            thread_yield();
            spin_lock(&s->lock);
        }
    }

    size_t copied = 0;
    while (copied < n && s->buf_head != s->buf_tail) {
        ((char *)buf)[copied] = s->buf[s->buf_head];
        s->buf_head = (s->buf_head + 1) % UNIX_SOCKET_BUF_SIZE;
        copied++;
    }

    spin_unlock(&s->lock);
    return copied;
}

static ssize_t sock_read(struct vnode *vp, void *buf, size_t n, off_t off) {
    (void)off;
    bool nonblock = false;
    struct proc *p = curproc;
    if (p) {
        for (int i = 0; i < MAX_FILES; i++) {
            struct file *f = p->p_fd_table[i];
            if (f && f->f_vn == vp) {
                if (f->f_flags & O_NONBLOCK) {
                    nonblock = true;
                    break;
                }
            }
        }
    }
    return sock_read_internal(vp, buf, n, nonblock);
}

static ssize_t sock_write(struct vnode *vp, const void *buf, size_t n, off_t off) {
    (void)off;
    struct unix_socket *s = (struct unix_socket *)vp->data;
    if (!s) return -EINVAL;

    spin_lock(&s->lock);
    struct unix_socket *peer = s->peer;
    if (!peer || peer->state != SS_CONNECTED) {
        spin_unlock(&s->lock);
        return -EPIPE; 
    }
    spin_unlock(&s->lock);

    spin_lock(&peer->lock);
    size_t written = 0;
    while (written < n) {
        uint32_t next_tail = (peer->buf_tail + 1) % UNIX_SOCKET_BUF_SIZE;
        if (next_tail == peer->buf_head) {
            
            spin_unlock(&peer->lock);
            thread_yield();
            spin_lock(&peer->lock);
            if (peer->state != SS_CONNECTED) {
                return -EPIPE;
            }
            continue;
        }

        peer->buf[peer->buf_tail] = ((const char *)buf)[written];
        peer->buf_tail = next_tail;
        written++;
    }
    spin_unlock(&peer->lock);

    return written;
}

static int sock_close(struct vnode *vp) {
    struct unix_socket *s = (struct unix_socket *)vp->data;
    if (!s) return -EINVAL;

    
    spin_lock(&s->lock);
    s->state = SS_DISCONNECTED;
    struct unix_socket *peer = s->peer;
    if (peer) {
        spin_lock(&peer->lock);
        peer->peer = NULL;
        peer->state = SS_DISCONNECTED;
        spin_unlock(&peer->lock);
        s->peer = NULL;
    }
    spin_unlock(&s->lock);

    
    ensure_global_lock_init();
    spin_lock(&bound_sockets_lock);
    for (int i = 0; i < MAX_BOUND_SOCKETS; i++) {
        if (bound_sockets[i] == s) {
            bound_sockets[i] = NULL;
            break;
        }
    }
    spin_unlock(&bound_sockets_lock);

    sock_free(s);
    return 0;
}


int64_t sys_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain == AF_NETLINK) {
        struct netlink_socket *ns = kmalloc(sizeof(struct netlink_socket));
        if (!ns) return -ENOMEM;
        memset(ns, 0, sizeof(struct netlink_socket));
        spin_lock_init(&ns->lock);

        if (type & O_NONBLOCK) {
            ns->nonblock = true;
        }

        struct vnode *vn = vnode_alloc(S_IFSOCK, &netlink_socket_ops);
        if (!vn) {
            kfree(ns);
            return -ENOMEM;
        }
        vn->data = ns;

        struct file *f = file_alloc();
        if (!f) {
            vput(vn);
            return -ENOMEM;
        }
        f->f_vn = vn;
        f->f_flags = O_RDWR;
        if (type & O_NONBLOCK) {
            f->f_flags |= O_NONBLOCK;
        }
        if (type & 0x80000) { // SOCK_CLOEXEC
            f->f_flags |= 0x80000;
        }

        int fd = proc_alloc_fd(curproc, f);
        if (fd < 0) {
            file_close(f);
            return fd;
        }

        return fd;
    }

    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if ((type & 0xF) != SOCK_STREAM) return -EINVAL;

    struct unix_socket *s = sock_alloc();
    if (!s) return -ENOMEM;

    struct vnode *vn = vnode_alloc(S_IFSOCK, &unix_socket_ops);
    if (!vn) {
        sock_free(s);
        return -ENOMEM;
    }
    vn->data = s;

    struct file *f = file_alloc();
    if (!f) {
        vput(vn);
        sock_free(s);
        return -ENOMEM;
    }
    f->f_vn = vn;
    f->f_flags = O_RDWR;
    if (type & O_NONBLOCK) {
        f->f_flags |= O_NONBLOCK;
    }
    if (type & 0x80000) { // SOCK_CLOEXEC
        f->f_flags |= 0x80000;
    }

    int fd = proc_alloc_fd(curproc, f);
    if (fd < 0) {
        file_close(f);
        return fd;
    }

    return fd;
}


int64_t sys_bind(int fd, const void *user_addr, uint32_t addrlen) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = curproc->p_fd_table[fd];
    if (!f || !f->f_vn) return -ENOTSOCK;

    if (f->f_vn->ops == &netlink_socket_ops) {
        struct netlink_socket *ns = (struct netlink_socket *)f->f_vn->data;
        if (!ns) return -EINVAL;
        struct sockaddr_nl addr;
        if (addrlen > sizeof(struct sockaddr_nl)) addrlen = sizeof(struct sockaddr_nl);
        if (copy_from_user(&addr, user_addr, addrlen) < 0) return -EFAULT;
        if (addr.nl_family != AF_NETLINK) return -EAFNOSUPPORT;

        spin_lock(&ns->lock);
        ns->bound_group = addr.nl_groups;
        spin_unlock(&ns->lock);
        return 0;
    }

    if (f->f_vn->ops != &unix_socket_ops) return -ENOTSOCK;

    struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
    if (!s) return -EINVAL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    if (addrlen > sizeof(struct sockaddr_un)) addrlen = sizeof(struct sockaddr_un);
    if (copy_from_user(&addr, user_addr, addrlen) < 0) return -EFAULT;

    if (addr.sun_family != AF_UNIX) return -EAFNOSUPPORT;

    spin_lock(&s->lock);
    if (s->state != SS_CLOSED) {
        spin_unlock(&s->lock);
        return -EINVAL;
    }

    strncpy(s->path, addr.sun_path, sizeof(s->path) - 1);
    s->state = SS_BOUND;
    spin_unlock(&s->lock);

    
    ensure_global_lock_init();
    spin_lock(&bound_sockets_lock);
    int slot = -1;
    for (int i = 0; i < MAX_BOUND_SOCKETS; i++) {
        if (bound_sockets[i] && strcmp(bound_sockets[i]->path, s->path) == 0) {
            spin_unlock(&bound_sockets_lock);
            return -EADDRINUSE;
        }
        if (!bound_sockets[i] && slot == -1) {
            slot = i;
        }
    }

    if (slot == -1) {
        spin_unlock(&bound_sockets_lock);
        return -ENOMEM;
    }

    bound_sockets[slot] = s;
    spin_unlock(&bound_sockets_lock);

    return 0;
}


int64_t sys_listen(int fd, int backlog) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = curproc->p_fd_table[fd];
    if (!f || !f->f_vn || f->f_vn->ops != &unix_socket_ops) return -ENOTSOCK;

    struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
    if (!s) return -EINVAL;

    spin_lock(&s->lock);
    if (s->state != SS_BOUND) {
        spin_unlock(&s->lock);
        return -EINVAL;
    }

    s->state = SS_LISTENING;
    s->backlog_head = 0;
    s->backlog_tail = 0;
    (void)backlog;
    spin_unlock(&s->lock);

    return 0;
}


int64_t sys_accept(int fd, void *user_addr, uint32_t *user_addrlen) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = curproc->p_fd_table[fd];
    if (!f || !f->f_vn || f->f_vn->ops != &unix_socket_ops) return -ENOTSOCK;

    struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
    if (!s) return -EINVAL;

    spin_lock(&s->lock);
    if (s->state != SS_LISTENING) {
        spin_unlock(&s->lock);
        return -EINVAL;
    }

    
    if (s->backlog_head == s->backlog_tail) {
        if (f->f_flags & O_NONBLOCK) {
            spin_unlock(&s->lock);
            return -EAGAIN;
        }
        while (s->backlog_head == s->backlog_tail) {
            spin_unlock(&s->lock);
            thread_yield();
            spin_lock(&s->lock);
            if (s->state != SS_LISTENING) {
                spin_unlock(&s->lock);
                return -EINVAL;
            }
        }
    }

    
    struct unix_socket *client_sock = s->backlog[s->backlog_head];
    s->backlog_head = (s->backlog_head + 1) % UNIX_SOCKET_MAX_BACKLOG;
    spin_unlock(&s->lock);

    
    struct unix_socket *server_chan = sock_alloc();
    if (!server_chan) return -ENOMEM;

    spin_lock(&server_chan->lock);
    spin_lock(&client_sock->lock);

    server_chan->peer = client_sock;
    server_chan->state = SS_CONNECTED;
    
    client_sock->peer = server_chan;
    client_sock->state = SS_CONNECTED;

    spin_unlock(&client_sock->lock);
    spin_unlock(&server_chan->lock);

    
    struct vnode *vn = vnode_alloc(S_IFSOCK, &unix_socket_ops);
    if (!vn) {
        sock_free(server_chan);
        return -ENOMEM;
    }
    vn->data = server_chan;

    struct file *chan_file = file_alloc();
    if (!chan_file) {
        vput(vn);
        sock_free(server_chan);
        return -ENOMEM;
    }
    chan_file->f_vn = vn;
    chan_file->f_flags = O_RDWR;

    int new_fd = proc_alloc_fd(curproc, chan_file);
    if (new_fd < 0) {
        file_close(chan_file);
        return new_fd;
    }

    
    if (user_addr && user_addrlen) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, s->path, sizeof(addr.sun_path) - 1);
        copy_to_user(user_addr, &addr, sizeof(struct sockaddr_un));
        uint32_t len = sizeof(struct sockaddr_un);
        copy_to_user(user_addrlen, &len, sizeof(uint32_t));
    }

    return new_fd;
}


int64_t sys_connect(int fd, const void *user_addr, uint32_t addrlen) {
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = curproc->p_fd_table[fd];
    if (!f || !f->f_vn || f->f_vn->ops != &unix_socket_ops) return -ENOTSOCK;

    struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
    if (!s) return -EINVAL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    if (addrlen > sizeof(struct sockaddr_un)) addrlen = sizeof(struct sockaddr_un);
    if (copy_from_user(&addr, user_addr, addrlen) < 0) return -EFAULT;

    if (addr.sun_family != AF_UNIX) return -EAFNOSUPPORT;

    
    struct unix_socket *server = NULL;
    ensure_global_lock_init();
    spin_lock(&bound_sockets_lock);
    for (int i = 0; i < MAX_BOUND_SOCKETS; i++) {
        if (bound_sockets[i] && strcmp(bound_sockets[i]->path, addr.sun_path) == 0) {
            server = bound_sockets[i];
            break;
        }
    }
    spin_unlock(&bound_sockets_lock);

    if (!server) {
        dprintf("[sys_connect] Connection refused: no socket found bound to '%s'\n", addr.sun_path);
        spin_lock(&bound_sockets_lock);
        for (int i = 0; i < MAX_BOUND_SOCKETS; i++) {
            if (bound_sockets[i]) {
                dprintf("  Bound socket %d: '%s'\n", i, bound_sockets[i]->path);
            }
        }
        spin_unlock(&bound_sockets_lock);
        return -ECONNREFUSED;
    }

    spin_lock(&server->lock);
    if (server->state != SS_LISTENING) {
        dprintf("[sys_connect] Connection refused: server socket is not listening (state: %d)\n", server->state);
        spin_unlock(&server->lock);
        return -ECONNREFUSED;
    }

    
    uint32_t next_tail = (server->backlog_tail + 1) % UNIX_SOCKET_MAX_BACKLOG;
    if (next_tail == (uint32_t)server->backlog_head) {
        spin_unlock(&server->lock);
        return -ECONNREFUSED;
    }

    server->backlog[server->backlog_tail] = s;
    server->backlog_tail = next_tail;
    spin_unlock(&server->lock);
    
    spin_lock(&s->lock);
    while (s->state != SS_CONNECTED) {
        spin_unlock(&s->lock);
        thread_yield();
        spin_lock(&s->lock);
        if (s->state == SS_DISCONNECTED) {
            spin_unlock(&s->lock);
            return -ECONNREFUSED;
        }
    }
    spin_unlock(&s->lock);

    return 0;
}


int64_t sys_sendmsg(int fd, const void *user_msg, int flags) {
    (void)flags;
    struct file *f = fdget(fd);
    int64_t ret = 0;
    if (!f) return -EBADF;
    if (!f->f_vn) { fdput(f); return -ENOTSOCK; }
    if (f->f_vn->ops == &netlink_socket_ops) { fdput(f); return -EINVAL; }
    if (f->f_vn->ops != &unix_socket_ops) { fdput(f); return -ENOTSOCK; }

    struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
    if (!s) { ret = -EINVAL; goto out; }

    struct msghdr msg;
    if (copy_from_user(&msg, user_msg, sizeof(struct msghdr)) < 0) { ret = -EFAULT; goto out; }

    spin_lock(&s->lock);
    struct unix_socket *peer = s->peer;
    if (!peer || peer->state != SS_CONNECTED) {
        spin_unlock(&s->lock);
        ret = -EPIPE; goto out;
    }
    spin_unlock(&s->lock);

    
    if (msg.msg_control && msg.msg_controllen >= sizeof(struct cmsghdr)) {
        struct cmsghdr cmsg;
        if (copy_from_user(&cmsg, msg.msg_control, sizeof(struct cmsghdr)) == 0) {
            if (cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS) {
                int fds_cnt = (cmsg.cmsg_len - sizeof(struct cmsghdr)) / sizeof(int);
                int *user_fds_buf = (int *)((char *)msg.msg_control + sizeof(struct cmsghdr));
                
                int local_fds[64];
                int local_n = 0;
                for (int i = 0; i < fds_cnt && i < 64; i++) {
                    int passed_fd;
                    if (copy_from_user(&passed_fd, &user_fds_buf[i], sizeof(int)) == 0) {
                        local_fds[local_n++] = passed_fd;
                    }
                }

                struct file *local_files[64];
                int local_nf = 0;
                for (int i = 0; i < local_n; i++) {
                    int passed_fd = local_fds[i];
                    if (passed_fd >= 0 && passed_fd < MAX_FILES) {
                        uint64_t _pflags = spin_lock_irqsave(&curproc->p_lock);
                        struct file *pf = curproc->p_fd_table[passed_fd];
                        if (pf) __atomic_fetch_add(&pf->f_refcnt, 1, __ATOMIC_SEQ_CST);
                        spin_unlock_irqrestore(&curproc->p_lock, _pflags);
                        local_files[local_nf++] = pf;
                    }
                }

                spin_lock(&peer->lock);
                for (int i = 0; i < local_nf; i++) {
                    struct file *pf = local_files[i];
                    if (pf) {
                        uint32_t next_passed_tail = (peer->passed_tail + 1) % UNIX_SOCKET_MAX_PASSED_FDS;
                        if (next_passed_tail != (uint32_t)peer->passed_head) {
                            peer->passed_files[peer->passed_tail] = pf;
                            peer->passed_tail = next_passed_tail;
                        } else {
                            file_close(pf); 
                        }
                    }
                }
                spin_unlock(&peer->lock);
            }
        }
    }

    
    size_t total_written = 0;
    if (msg.msg_iov && msg.msg_iovlen > 0) {
        struct iovec iov;
        for (uint64_t i = 0; i < msg.msg_iovlen; i++) {
            if (copy_from_user(&iov, &msg.msg_iov[i], sizeof(struct iovec)) < 0) { ret = -EFAULT; goto out; }
            if (iov.iov_len == 0) continue;

            size_t written = 0;
            while (written < iov.iov_len) {
                char b;
                if (copy_from_user(&b, (char *)iov.iov_base + written, 1) < 0) {
                    ret = total_written > 0 ? (int64_t)total_written : -EFAULT;
                    goto out;
                }

                spin_lock(&peer->lock);

                while (1) {
                    if (peer->state != SS_CONNECTED) {
                        spin_unlock(&peer->lock);
                        ret = total_written > 0 ? (int64_t)total_written : -EPIPE;
                        goto out;
                    }
                    uint32_t next_tail = (peer->buf_tail + 1) % UNIX_SOCKET_BUF_SIZE;
                    if (next_tail != peer->buf_head)
                        break;
                    spin_unlock(&peer->lock);
                    thread_yield();
                    spin_lock(&peer->lock);
                }

                uint32_t next_tail = (peer->buf_tail + 1) % UNIX_SOCKET_BUF_SIZE;
                peer->buf[peer->buf_tail] = b;
                peer->buf_tail = next_tail;
                written++;
                total_written++;
                spin_unlock(&peer->lock);
            }
        }
    }

    ret = (int64_t)total_written;
out:
    fdput(f);
    return ret;
}


int64_t sys_recvmsg(int fd, void *user_msg, int flags) {
    (void)flags;
    struct file *f = fdget(fd);
    if (!f) return -EBADF;
    if (!f->f_vn) { fdput(f); return -ENOTSOCK; }
    if (f->f_vn->ops == &netlink_socket_ops) {
        fdput(f); return -EAGAIN;
    }
    if (f->f_vn->ops != &unix_socket_ops) { fdput(f); return -ENOTSOCK; }

    int64_t ret = 0;

    struct unix_socket *s = (struct unix_socket *)f->f_vn->data;
    if (!s) { ret = -EINVAL; goto out; }

    struct msghdr msg;
    if (copy_from_user(&msg, user_msg, sizeof(struct msghdr)) < 0) { ret = -EFAULT; goto out; }

    spin_lock(&s->lock);
    if (s->state != SS_CONNECTED && s->buf_head == s->buf_tail) {
        spin_unlock(&s->lock);
        ret = 0; goto out; 
    }

    while (s->buf_head == s->buf_tail && s->state == SS_CONNECTED) {
        spin_unlock(&s->lock);
        thread_yield();
        spin_lock(&s->lock);
    }
    spin_unlock(&s->lock);

    size_t total_read = 0;
    if (msg.msg_iov && msg.msg_iovlen > 0) {
        struct iovec iov;
        for (uint64_t i = 0; i < msg.msg_iovlen; i++) {
            if (copy_from_user(&iov, &msg.msg_iov[i], sizeof(struct iovec)) < 0) { ret = -EFAULT; goto out; }
            if (iov.iov_len == 0) continue;

            spin_lock(&s->lock);
            size_t read_bytes = 0;
            while (read_bytes < iov.iov_len && s->buf_head != s->buf_tail) {
                char b = s->buf[s->buf_head];
                s->buf_head = (s->buf_head + 1) % UNIX_SOCKET_BUF_SIZE;
                spin_unlock(&s->lock);
                if (copy_to_user((char *)iov.iov_base + read_bytes, &b, 1) < 0) {
                    ret = -EFAULT; goto out;
                }
                spin_lock(&s->lock);
                read_bytes++;
                total_read++;
            }
            spin_unlock(&s->lock);
            if (read_bytes < iov.iov_len) break; 
        }
    }

    if (total_read > 0 && msg.msg_control && msg.msg_controllen >= sizeof(struct cmsghdr)) {
        struct file *extracted_files[64];
        int extracted_cnt = 0;
        int max_to_pass = (msg.msg_controllen - sizeof(struct cmsghdr)) / sizeof(int);
        if (max_to_pass > 64) max_to_pass = 64;

        spin_lock(&s->lock);
        while (s->passed_head != s->passed_tail && extracted_cnt < max_to_pass) {
            struct file *pf = s->passed_files[s->passed_head];
            s->passed_files[s->passed_head] = NULL;
            s->passed_head = (s->passed_head + 1) % UNIX_SOCKET_MAX_PASSED_FDS;
            extracted_files[extracted_cnt++] = pf;
        }
        spin_unlock(&s->lock);

        if (extracted_cnt > 0) {
            struct cmsghdr cmsg;
            cmsg.cmsg_level = SOL_SOCKET;
            cmsg.cmsg_type = SCM_RIGHTS;

            int passed_cnt = 0;
            int *user_fds_buf = (int *)((char *)msg.msg_control + sizeof(struct cmsghdr));
            int new_fds[64];

            for (int i = 0; i < extracted_cnt; i++) {
                int new_fd = proc_alloc_fd(curproc, extracted_files[i]);
                if (new_fd >= 0) {
                    new_fds[passed_cnt++] = new_fd;
                } else {
                    file_close(extracted_files[i]);
                }
            }

            for (int i = 0; i < passed_cnt; i++) {
                copy_to_user(&user_fds_buf[i], &new_fds[i], sizeof(int));
            }

            cmsg.cmsg_len = sizeof(struct cmsghdr) + passed_cnt * sizeof(int);
            copy_to_user(msg.msg_control, &cmsg, sizeof(struct cmsghdr));
            msg.msg_controllen = cmsg.cmsg_len;
            copy_to_user(user_msg, &msg, sizeof(struct msghdr));
        }
    }

    ret = (int64_t)total_read;
out:
    fdput(f);
    return ret;
}


int64_t sys_sendto(int fd, const void *user_buf, size_t len, int flags, const void *user_dest_addr, uint32_t addrlen) {
    (void)flags;
    (void)user_dest_addr;
    (void)addrlen;

    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = curproc->p_fd_table[fd];
    if (!f || !f->f_vn) return -ENOTSOCK;
    if (f->f_vn->ops == &netlink_socket_ops) return -EINVAL;
    if (f->f_vn->ops != &unix_socket_ops) return -ENOTSOCK;

    if (len == 0) return 0;

    size_t total_written = 0;
    char chunk[4096];
    while (total_written < len) {
        size_t to_write = len - total_written;
        if (to_write > sizeof(chunk)) to_write = sizeof(chunk);

        if (copy_from_user(chunk, (const char *)user_buf + total_written, to_write) < 0) {
            return total_written > 0 ? (int64_t)total_written : -EFAULT;
        }

        ssize_t ret = sock_write(f->f_vn, chunk, to_write, 0);
        if (ret < 0) {
            return total_written > 0 ? (int64_t)total_written : ret;
        }
        total_written += ret;
        if (ret < (ssize_t)to_write) {
            break; 
        }
    }

    return (int64_t)total_written;
}


int64_t sys_recvfrom(int fd, void *user_buf, size_t len, int flags, void *user_src_addr, uint32_t *user_addrlen) {
    (void)user_src_addr;
    (void)user_addrlen;

    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    struct file *f = curproc->p_fd_table[fd];
    if (!f || !f->f_vn) return -ENOTSOCK;
    if (f->f_vn->ops == &netlink_socket_ops) return -EAGAIN;
    if (f->f_vn->ops != &unix_socket_ops) return -ENOTSOCK;

    if (len == 0) return 0;

    bool nonblock = (f->f_flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);

    size_t total_read = 0;
    char chunk[4096];
    while (total_read < len) {
        size_t to_read = len - total_read;
        if (to_read > sizeof(chunk)) to_read = sizeof(chunk);

        ssize_t ret = sock_read_internal(f->f_vn, chunk, to_read, nonblock);
        if (ret < 0) {
            if (ret != -EAGAIN) {
            }
            return total_read > 0 ? (int64_t)total_read : ret;
        }
        if (ret == 0) {
            break; 
        }

        if (copy_to_user((char *)user_buf + total_read, chunk, ret) < 0) {
            return total_read > 0 ? (int64_t)total_read : -EFAULT;
        }
        total_read += ret;
        if (ret < (ssize_t)to_read) {
            break; 
        }
    }

    return (int64_t)total_read;
}

int64_t sys_socketpair(int domain, int type, int protocol, int *user_sv) {
    (void)protocol;
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    int base_type = type & 0xf;
    if (base_type != SOCK_STREAM) return -EINVAL;

    if (!is_user_address_range(user_sv, sizeof(int) * 2)) {
        return -EFAULT;
    }

    struct unix_socket *s1 = sock_alloc();
    if (!s1) return -ENOMEM;

    struct unix_socket *s2 = sock_alloc();
    if (!s2) {
        sock_free(s1);
        return -ENOMEM;
    }

    s1->peer = s2;
    s1->state = SS_CONNECTED;
    s2->peer = s1;
    s2->state = SS_CONNECTED;

    struct vnode *vn1 = vnode_alloc(S_IFSOCK, &unix_socket_ops);
    if (!vn1) {
        sock_free(s1);
        sock_free(s2);
        return -ENOMEM;
    }
    vn1->data = s1;

    struct vnode *vn2 = vnode_alloc(S_IFSOCK, &unix_socket_ops);
    if (!vn2) {
        vput(vn1);
        sock_free(s2);
        return -ENOMEM;
    }
    vn2->data = s2;

    struct file *f1 = file_alloc();
    if (!f1) {
        vput(vn1);
        vput(vn2);
        return -ENOMEM;
    }
    f1->f_vn = vn1;
    f1->f_flags = O_RDWR;
    if (type & O_NONBLOCK) {
        f1->f_flags |= O_NONBLOCK;
    }
    if (type & 0x80000) { // SOCK_CLOEXEC
        f1->f_flags |= 0x80000;
    }

    struct file *f2 = file_alloc();
    if (!f2) {
        file_close(f1);
        vput(vn2);
        return -ENOMEM;
    }
    f2->f_vn = vn2;
    f2->f_flags = O_RDWR;
    if (type & O_NONBLOCK) {
        f2->f_flags |= O_NONBLOCK;
    }
    if (type & 0x80000) { // SOCK_CLOEXEC
        f2->f_flags |= 0x80000;
    }

    int fd1 = proc_alloc_fd(curproc, f1);
    if (fd1 < 0) {
        file_close(f1);
        file_close(f2);
        return fd1;
    }

    int fd2 = proc_alloc_fd(curproc, f2);
    if (fd2 < 0) {
        curproc->p_fd_table[fd1] = NULL;
        file_close(f1);
        file_close(f2);
        return fd2;
    }

    int k_fds[2] = { fd1, fd2 };
    if (copy_to_user(user_sv, k_fds, sizeof(int) * 2) < 0) {
        curproc->p_fd_table[fd1] = NULL;
        curproc->p_fd_table[fd2] = NULL;
        file_close(f1);
        file_close(f2);
        return -EFAULT;
    }

    return 0;
}

int64_t sys_accept4(int fd, void *user_addr, uint32_t *user_addrlen, int flags) {
    int64_t ret = sys_accept(fd, user_addr, user_addrlen);
    if (ret >= 0) {
        int new_fd = (int)ret;
        struct file *f = curproc->p_fd_table[new_fd];
        if (f) {
            if (flags & 0x800) { // SOCK_NONBLOCK / O_NONBLOCK
                f->f_flags |= O_NONBLOCK;
            }
            if (flags & 0x80000) { // SOCK_CLOEXEC / O_CLOEXEC
                f->f_flags |= 0x80000;
            }
        }
    }
    return ret;
}


