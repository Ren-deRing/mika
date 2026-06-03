#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <signal.h>

void sigusr1_handler(int sig) {
    printf("[CHILD] Handler: oh signal %d! Back to you, parent.\n", sig);
}

int main(int argc, char *argv[]) {
    int fd_stdin = open("/dev/tty", O_RDONLY);
    int fd_stdout = open("/dev/tty", O_WRONLY);
    int fd_stderr = open("/dev/tty", O_WRONLY);

    printf("Hello MUSL!\n");
    printf("[INIT] Standard streams opened: stdin=%d, stdout=%d, stderr=%d\n", fd_stdin, fd_stdout, fd_stderr);

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

        printf("\n[FPU] Starting XSAVE Test...\n");
        pid_t fpu_pid = fork();
        if (fpu_pid < 0) {
            perror("[PARENT] FPU fork failed");
            return 1;
        }
        else if (fpu_pid == 0) {
            volatile double f = 10.0;
            printf("[CHILD] Start value: %f\n", f);
            
            for (int i = 0; i < 500; i++) {
                for (volatile int j = 0; j < 50000; j++);
                f = f + 0.1;
                if (i % 100 == 0) {
                    printf("[CHILD] Progress loop %d, f = %f\n", i, f);
                }
            }
            
            printf("[CHILD] Result: %f (Expected: ~60.000000)\n", f);

            exit(0);
        }
        else {
            volatile double f = 100.0;
            printf("[PARENT] Start value: %f\n", f);
            
            for (int i = 0; i < 500; i++) {
                for (volatile int j = 0; j < 50000; j++);
                f = f - 0.1;
                if (i % 100 == 0) {
                    printf("[PARENT] Progress loop %d, f = %f\n", i, f);
                }
            }
            
            printf("[PARENT] Result: %f (Expected: ~50.000000)\n", f);
            
            int fpu_status = 0;
            waitpid(fpu_pid, &fpu_status, 0);
            printf("[PARENT] BYE!\n");

            pid_t hello_pid = fork();
            if (hello_pid < 0) {
                perror("[PARENT] HELLO fork failed");
                return 1;
            }
            else if (hello_pid == 0) {        
                char *execve_argv[] = {"/bin/hello", "hello_arg1", "hello_arg2", NULL};
                char *execve_envp[] = {"PATH=/bin", "USER=heebb", "SHELL=/bin/sh", NULL};

                execve("/bin/hello", execve_argv, execve_envp);

                exit(0);
            }
            else {
                int hello_status = 0;
                waitpid(hello_pid, &hello_status, 0);
                printf("[PARENT] BYE!\n");
            }
        }
    }
    
    char *doom_argv[4];
    doom_argv[0] = "/bin/doom";
    
    printf("[INIT] Checking for WAD files...\n");
    int fd1 = open("/usr/share/games/doom/freedoom1.wad", 0);
    int fd2 = -1;
    if (fd1 < 0) {
        fd2 = open("/usr/share/games/doom/freedoom2.wad", 0);
    }

    if (fd1 >= 0) {
        close(fd1);
        printf("[INIT] Found Freedoom Phase 1 WAD!\n");
        doom_argv[1] = "-iwad";
        doom_argv[2] = "/usr/share/games/doom/freedoom1.wad";
        doom_argv[3] = NULL;
    } else if (fd2 >= 0) {
        close(fd2);
        printf("[INIT] Found Freedoom Phase 2 WAD!\n");
        doom_argv[1] = "-iwad";
        doom_argv[2] = "/usr/share/games/doom/freedoom2.wad";
        doom_argv[3] = NULL;
    } else {
        printf("[INIT] No Freedoom WAD found in standard location, let doom engine auto-detect.\n");
        doom_argv[1] = NULL;
    }

    char *doom_envp[] = {"PATH=/bin", "USER=heebb", "SHELL=/bin/sh", NULL};

    printf("[INIT] Launching DOOM...\n");
    execve("/bin/doom", doom_argv, doom_envp);

    return 0;
}