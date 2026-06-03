#pragma once

#include <uapi/sys/ipc.h>

struct semid_ds {
    struct ipc_perm sem_perm;    /* Ownership and permissions */
    time_t          sem_otime;   /* Last semop time */
    time_t          sem_ctime;   /* Last change time */
    unsigned long   sem_nsems;   /* No. of semaphores in set */
    uint64_t        __unused1;
    uint64_t        __unused2;
};

struct sembuf {
    unsigned short sem_num;  /* semaphore number */
    short          sem_op;   /* semaphore operation */
    short          sem_flg;  /* operation flags */
};

/* semop flags */
#define SEM_UNDO        0x1000  /* undo the operation on exit */

/* semctl commands */
#define GETPID          11      /* get sempid */
#define GETVAL          12      /* get semval */
#define GETALL          13      /* get all semvals */
#define GETNCNT         14      /* get semncnt */
#define GETZCNT         15      /* get semzcnt */
#define SETVAL          16      /* set semval */
#define SETALL          17      /* set all semvals */
