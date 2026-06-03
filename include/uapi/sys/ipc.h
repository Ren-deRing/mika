#pragma once

#include <uapi/types.h>

typedef int32_t key_t;

struct ipc_perm {
    key_t          __key;    /* Key supplied to semget(2)/shmget(2) */
    uid_t          uid;      /* Effective UID of owner */
    gid_t          gid;      /* Effective GID of owner */
    uid_t          cuid;     /* Effective UID of creator */
    gid_t          cgid;     /* Effective GID of creator */
    unsigned short mode;     /* Permissions */
    unsigned short __seq;    /* Sequence number */
};

#define IPC_CREAT  01000  /* Create entry if key does not exist */
#define IPC_EXCL   02000  /* Fail if key exists */
#define IPC_NOWAIT 04000  /* Error if request must wait */

#define IPC_RMID   0      /* Remove resource */
#define IPC_SET    1      /* Set resource parameters */
#define IPC_STAT   2      /* Get resource status */
#define IPC_INFO   3      /* IPC info */

#define IPC_PRIVATE ((key_t)0)
