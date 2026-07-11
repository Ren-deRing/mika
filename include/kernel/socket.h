#pragma once

#include <stdint.h>
#include <kernel/lock.h>
#include <kernel/wait.h>

struct file;
struct vnode;

#define UNIX_SOCKET_MAX_BACKLOG 16
#define UNIX_SOCKET_BUF_SIZE 65536
#define UNIX_SOCKET_MAX_PASSED_FDS 256

enum socket_state {
    SS_CLOSED,
    SS_BOUND,
    SS_LISTENING,
    SS_CONNECTED,
    SS_DISCONNECTED
};

struct unix_socket {
    enum socket_state state;
    char path[108];
    struct unix_socket *peer;
    spinlock_t lock;
    wait_queue_head_t rcv_wq;
    struct unix_socket *backlog[UNIX_SOCKET_MAX_BACKLOG];
    int backlog_head;
    int backlog_tail;
    char *buf;
    uint32_t buf_head;
    uint32_t buf_tail;
    struct file *passed_files[UNIX_SOCKET_MAX_PASSED_FDS];
    int passed_head;
    int passed_tail;
};
