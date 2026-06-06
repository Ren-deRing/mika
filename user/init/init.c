#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mount.h>

void sigusr1_handler(int sig) {
    printf("[CHILD] Handler: oh signal %d! Back to you, parent.\n", sig);
}

int main(int argc, char *argv[]) {
    int fd_stdin = open("/dev/tty", O_RDONLY);
    int fd_stdout = open("/dev/tty", O_WRONLY);
    int fd_stderr = open("/dev/tty", O_WRONLY);

    printf("Hello MUSL!\n");
    
    mkdir("/mnt/usr", 0755);
    mount("/usr", "/mnt/usr", "bind", 0, NULL);

    printf("[INIT] Launching DOOM directly...\n");
    pid_t doom_pid = fork();
    if (doom_pid < 0) {
        perror("[INIT] Fork for DOOM failed");
        return 1;
    }
    else if (doom_pid == 0) {
        char *doom_argv[] = {"/bin/doom", "-iwad", "/usr/share/games/doom/DOOM.WAD", NULL};
        char *doom_envp[] = {
            "PATH=/bin", 
            "USER=heebb", 
            "SHELL=/bin/sh", 
            NULL
        };
        execve("/bin/doom", doom_argv, doom_envp);
        perror("[INIT] execve /bin/doom failed");
        exit(1);
    }

    printf("[INIT] Startup complete. Entering main wait loop...\n");
    int status = 0;
    waitpid(doom_pid, &status, 0);

    return 0;
}