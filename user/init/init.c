#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mount.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int fd_stdin = open("/dev/tty", O_RDONLY);
    int fd_stdout = open("/dev/tty", O_WRONLY);
    int fd_stderr = open("/dev/tty", O_WRONLY);

    (void)fd_stdin;
    (void)fd_stdout;
    (void)fd_stderr;

    printf("[INIT] starting\n");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    mkdir("/mnt/usr", 0755);
    mount("/usr", "/mnt/usr", "bind", 0, NULL);

    pid_t ui_pid = fork();
    if (ui_pid == 0) {
        char *ui_argv[] = {"/bin/ui", NULL};
        char *ui_envp[] = {NULL};
        execve("/bin/ui", ui_argv, ui_envp);
        perror("[INIT] execve /bin/ui");
        _exit(127);
    }

    if (ui_pid < 0) {
        perror("[INIT] fork /bin/ui");
    } else {
        printf("[INIT] launched /bin/ui pid=%d\n", ui_pid);
    }

    while (1) {
        int status = 0;
        pid_t done = waitpid(ui_pid, &status, 0);
        if (done == ui_pid) {
            printf("[INIT] /bin/ui exited status=%d; restarting in 1s\n", status);
            sleep(1);
            ui_pid = fork();
            if (ui_pid == 0) {
                char *ui_argv[] = {"/bin/ui", NULL};
                char *ui_envp[] = {NULL};
                execve("/bin/ui", ui_argv, ui_envp);
                _exit(127);
            }
        } else {
            sleep(1);
        }
    }

    return 0;
}
