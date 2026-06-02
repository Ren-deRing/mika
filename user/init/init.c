#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <signal.h>

void sigusr1_handler(int sig) {
    printf("[CHILD] Handler: oh signal %d! Back to you, parent.\n", sig);
}

int main(int argc, char *argv[]) {
    printf("Hello MUSL!\n");

    pid_t parent_pid = getpid();
    printf("[PARENT] PID is: %d\n", parent_pid);
    printf("[PARENT] Forking a child process...\n");

    pid_t pid = fork();

    if (pid < 0) {
        perror("[PARENT] fork failed");
        return 1;
    } 
    else if (pid == 0) {
        pid_t child_pid = getpid();
        printf("[CHILD] My actual getpid() is: %d\n", child_pid);

        struct sigaction sa;
        sa.sa_handler = sigusr1_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_restorer = NULL;

        if (sigaction(SIGUSR1, &sa, NULL) < 0) {
            perror("[CHILD] sigaction failed");
            exit(1);
        }
        printf("[CHILD] SIGUSR1 handler registered!\n");
        
        printf("[CHILD] Waiting for parent's signal...\n");
        for (int i = 0; i < 10; i++) {
            for (volatile int j = 0; j < 20000000; j++);
            printf("[CHILD] I'm still alive (Loop %d)...\n", i);
        }

        exit(0);
    } 
    else {
        printf("[PARENT] fork() returned (Child PID): %d\n", pid);
        
        for (volatile int j = 0; j < 30000000; j++);
        
        printf("[PARENT] Sending SIGUSR1 (10) to Child...\n");
        kill(pid, SIGUSR1);

        for (volatile int j = 0; j < 50000000; j++);

        printf("[PARENT] Now sending SIGTERM (15) to Child...\n");
        kill(pid, SIGTERM);

        int status = 0;
        printf("[PARENT] Waiting for child via waitpid...\n");
        waitpid(pid, &status, 0);

        printf("[PARENT] Child raw exit status: %X\n", status);
        if (WIFEXITED(status)) {
            printf("[PARENT] Child exited normally with status: %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[PARENT] Child was terminated by signal: %d\n", WTERMSIG(status));
        } else {
            printf("[PARENT] Child terminated.\n");
        }
    }

    return 0;
}