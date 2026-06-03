#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static volatile int ready = 0;

static void *thread_func(void *arg) {
    (void)arg;
    printf("[TEST_CHILD] Hello from the child\n");

    printf("[TEST_CHILD] Anyway, I have to do something great, but in this kernel, it dosen't exist. so I guess I'll just have to waste some time.\n");
    for (volatile int i = 0; i < 10000000; i++) {
        // spin
    }

    printf("[TEST_CHILD] Child acquiring mutex to signal readiness...\n");
    pthread_mutex_lock(&mutex);
    ready = 1;
    printf("[TEST_CHILD] Child signaling condition variable...\n");
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);

    printf("[TEST_CHILD] Child thread exiting and returning 42...\n");
    return (void *)42;
}

static void custom_sigsegv_handler(int sig) {
    printf("[SIGNAL_TEST] Caught custom SIGSEGV (signal %d)!\n", sig);
    exit(12);
}

static void custom_sigfpe_handler(int sig) {
    printf("[SIGNAL_TEST] Caught custom SIGFPE (signal %d)!\n", sig);
    exit(13);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    pthread_t thread;

    printf("\n");

    printf("[TEST_PARENT] Creating child thread...\n");
    int ret = pthread_create(&thread, NULL, thread_func, NULL);
    if (ret != 0) {
        printf("[TEST_PARENT] pthread_create failed with error code: %d\n", ret);
        return 1;
    }

    printf("[TEST_PARENT] Thread spawned successfully. Waiting on cond var...\n");
    
    pthread_mutex_lock(&mutex);
    while (!ready) {
        printf("[TEST_PARENT] ready is %d. Sleeping on cond var...\n", ready);
        pthread_cond_wait(&cond, &mutex);
    }
    printf("[TEST_PARENT] Successfully woken up! ready is %d\n", ready);
    pthread_mutex_unlock(&mutex);

    printf("[TEST_PARENT] Joining child thread...\n");
    void *retval = NULL;
    ret = pthread_join(thread, &retval);
    if (ret != 0) {
        printf("[TEST_PARENT] pthread_join failed with error code: %d\n", ret);
        return 1;
    }

    printf("[TEST_PARENT] Thread joined, Return value: %ld\n", (long)retval);

    printf("\n[SIGNAL_TEST] Starting Exception-to-Signal Tests...\n");

    printf("[SIGNAL_TEST] Test 1: Triggering unhandled SIGSEGV in child...\n");
    pid_t pid1 = fork();
    if (pid1 < 0) {
        perror("fork");
        return 1;
    } else if (pid1 == 0) {
        printf("[CHILD_1] Dereferencing NULL pointer now...\n");
        volatile int *ptr = NULL;
        (void)*ptr;
        printf("[CHILD_1] This should not be printed!\n");
        exit(0);
    } else {
        int status;
        waitpid(pid1, &status, 0);
        printf("[PARENT] Child 1 status: %X\n", status);
        if (WIFSIGNALED(status)) {
            printf("[PARENT] Child 1 was terminated by signal: %d (Expected: %d/SIGSEGV)\n", WTERMSIG(status), SIGSEGV);
        } else if (WIFEXITED(status)) {
            printf("[PARENT] Child 1 exited with status: %d\n", WEXITSTATUS(status));
        }
    }

    printf("\n[SIGNAL_TEST] Test 2: Triggering unhandled SIGFPE in child...\n");
    pid_t pid2 = fork();
    if (pid2 < 0) {
        perror("fork");
        return 1;
    } else if (pid2 == 0) {
        printf("[CHILD_2] Dividing by zero now...\n");
        volatile int a = 42;
        volatile int b = 0;
        volatile int c = a / b;
        (void)c;
        printf("[CHILD_2] This should not be printed!\n");
        exit(0);
    } else {
        int status;
        waitpid(pid2, &status, 0);
        printf("[PARENT] Child 2 status: %X\n", status);
        if (WIFSIGNALED(status)) {
            printf("[PARENT] Child 2 was terminated by signal: %d (Expected: %d/SIGFPE)\n", WTERMSIG(status), SIGFPE);
        } else if (WIFEXITED(status)) {
            printf("[PARENT] Child 2 exited with status: %d\n", WEXITSTATUS(status));
        }
    }

    printf("\n[SIGNAL_TEST] Test 3: Custom SIGSEGV handler...\n");
    pid_t pid3 = fork();
    if (pid3 < 0) {
        perror("fork");
        return 1;
    } else if (pid3 == 0) {
        struct sigaction sa;
        sa.sa_handler = custom_sigsegv_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_restorer = NULL;
        if (sigaction(SIGSEGV, &sa, NULL) < 0) {
            perror("sigaction");
            exit(1);
        }
        printf("[CHILD_3] Custom SIGSEGV handler registered. Dereferencing NULL...\n");
        volatile int *ptr = NULL;
        (void)*ptr;
        exit(0);
    } else {
        int status;
        waitpid(pid3, &status, 0);
        printf("[PARENT] Child 3 status: %X\n", status);
        if (WIFEXITED(status)) {
            printf("[PARENT] Child 3 exited with status: %d (Expected: 12)\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[PARENT] Child 3 terminated by signal: %d\n", WTERMSIG(status));
        }
    }

    printf("\n[SIGNAL_TEST] Test 4: Custom SIGFPE handler...\n");
    pid_t pid4 = fork();
    if (pid4 < 0) {
        perror("fork");
        return 1;
    } else if (pid4 == 0) {
        struct sigaction sa;
        sa.sa_handler = custom_sigfpe_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_restorer = NULL;
        if (sigaction(SIGFPE, &sa, NULL) < 0) {
            perror("sigaction");
            exit(1);
        }
        printf("[CHILD_4] Custom SIGFPE handler registered. Dividing by zero...\n");
        volatile int a = 100;
        volatile int b = 0;
        volatile int c = a / b;
        (void)c;
        exit(0);
    } else {
        int status;
        waitpid(pid4, &status, 0);
        printf("[PARENT] Child 4 status: %X\n", status);
        if (WIFEXITED(status)) {
            printf("[PARENT] Child 4 exited with status: %d (Expected: 13)\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[PARENT] Child 4 terminated by signal: %d\n", WTERMSIG(status));
        }
    }

    printf("[TEST] BYE!\n");

    char *execve_argv[] = {"/bin/shm_sem_test", "hello_arg1", "hello_arg2", NULL};
    char *execve_envp[] = {"PATH=/bin", "USER=heebb", "SHELL=/bin/sh", NULL};

    execve("/bin/shm_sem_test", execve_argv, execve_envp);
    return 0;
}
