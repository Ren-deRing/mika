#pragma once

#include <uapi/sys/ipc.h>

typedef uint64_t shmatt_t;

struct shmid_ds {
    struct ipc_perm shm_perm;    /* Ownership and permissions */
    size_t          shm_segsz;   /* Size of segment (bytes) */
    time_t          shm_atime;   /* Last attach time */
    time_t          shm_dtime;   /* Last detach time */
    time_t          shm_ctime;   /* Last change time */
    pid_t           shm_cpid;    /* PID of creator */
    pid_t           shm_lpid;    /* PID of last shmat(2)/shmdt(2) */
    shmatt_t        shm_nattch;  /* No. of current attaches */
    uint64_t        __unused1;
    uint64_t        __unused2;
};

/* Flags for shmat */
#define SHM_RDONLY      010000  /* attach read-only else read-write */
#define SHM_RND         020000  /* round attach address to SHMLBA boundary */
#define SHM_REMAP       040000  /* take-over region on attach */
#define SHM_EXEC        0100000 /* execution access */

#define SHMLBA          4096    /* Segment low boundary address multiple */
