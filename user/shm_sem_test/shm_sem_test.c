#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>

#define SHM_KEY 0x1234
#define SEM_KEY 0x5678

#ifndef SYS_shmget
#define SYS_shmget 29
#endif
#ifndef SYS_shmat
#define SYS_shmat 30
#endif
#ifndef SYS_shmctl
#define SYS_shmctl 31
#endif
#ifndef SYS_semget
#define SYS_semget 64
#endif
#ifndef SYS_semop
#define SYS_semop 65
#endif
#ifndef SYS_semctl
#define SYS_semctl 66
#endif
#ifndef SYS_shmdt
#define SYS_shmdt 67
#endif

int main() {
    int shmid = syscall(SYS_shmget, SHM_KEY, 4096, IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget failed");
        return 1;
    }
    printf("[shm_sem_test] Created Shared Memory segment, shmid: %d\n", shmid);

    int semid = syscall(SYS_semget, SEM_KEY, 1, IPC_CREAT | 0666);
    if (semid < 0) {
        perror("semget failed");
        return 1;
    }
    printf("[shm_sem_test] Created Semaphore set, semid: %d\n", semid);

    if (syscall(SYS_semctl, semid, 0, SETVAL, 0) < 0) {
        perror("semctl SETVAL failed");
        return 1;
    }
    printf("[shm_sem_test] Initialized Semaphore value to 0\n");

    volatile unsigned int *shm_ptr = (volatile unsigned int *)syscall(SYS_shmat, shmid, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("shmat failed in parent");
        return 1;
    }
    printf("[shm_sem_test] Attached Shared Memory in parent at address: %p\n", shm_ptr);

    *shm_ptr = 0xDEADBEEF;
    printf("[shm_sem_test] Parent wrote signature: 0xDEADBEEF\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return 1;
    }

    if (pid == 0) {
        printf("[shm_sem_test] [Child] Started.\n");

        volatile unsigned int *child_shm_ptr = (volatile unsigned int *)syscall(SYS_shmat, shmid, NULL, 0);
        if (child_shm_ptr == (void *)-1) {
            perror("[shm_sem_test] [Child] shmat failed");
            exit(1);
        }
        printf("[shm_sem_test] [Child] Attached Shared Memory at address: %p\n", child_shm_ptr);

        if (*child_shm_ptr != 0xDEADBEEF) {
            printf("[shm_sem_test] [Child] ERROR: Found signature: 0x%X (expected 0xDEADBEEF)\n", *child_shm_ptr);
            exit(1);
        }
        printf("[shm_sem_test] [Child] Parent signature: 0xDEADBEEF\n");

        *child_shm_ptr = 0xCAFEBABE;
        printf("[shm_sem_test] [Child] Modified signature to: 0xCAFEBABE\n");

        syscall(SYS_shmdt, child_shm_ptr);
        printf("[shm_sem_test] [Child] Detached from shared memory.\n");

        struct sembuf sop;
        sop.sem_num = 0;
        sop.sem_op = 1; // +1 (V)
        sop.sem_flg = 0;
        printf("[shm_sem_test] [Child] Posting semaphore...\n");
        if (syscall(SYS_semop, semid, &sop, 1) < 0) {
            perror("[shm_sem_test] [Child] semop fail");
            exit(1);
        }
        printf("[shm_sem_test] [Child] Exiting.\n");
        exit(0);
    } else {
        printf("[shm_sem_test] [Parent] Waiting on Semaphore to be posted by Child...\n");

        struct sembuf sop;
        sop.sem_num = 0;
        sop.sem_op = -1; // -1 (P, sem 값 >= 1까지 대기)
        sop.sem_flg = 0;
        if (syscall(SYS_semop, semid, &sop, 1) < 0) {
            perror("[shm_sem_test] [Parent] semop fail");
            return 1;
        }
        printf("[shm_sem_test] [Parent] Semaphore posted! Verifying Child modifications...\n");

        if (*shm_ptr != 0xCAFEBABE) {
            printf("[shm_sem_test] [Parent] ERROR: Found signature: 0x%X (expected 0xCAFEBABE)\n", *shm_ptr);
            return 1;
        }
        printf("[shm_sem_test] [Parent] Child signature: 0xCAFEBABE!\n");

        syscall(SYS_shmdt, (void *)shm_ptr);
        printf("[shm_sem_test] [Parent] Detached shared memory.\n");

        syscall(SYS_shmctl, shmid, IPC_RMID, NULL);
        syscall(SYS_semctl, semid, 0, IPC_RMID, 0);
        printf("[shm_sem_test] [Parent] Cleaned up IPC resources.\n");

        printf("[shm_sem_test] PASSED\n");
    }

    return 0;
}
