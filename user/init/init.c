#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/stat.h>
#include <dirent.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/shm.h>

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

#define NUM_CHILDREN     4
#define ITERATIONS       500
#define SHM_SIZE  (4UL * 1024 * 1024)
#define TIMEOUT_SEC      50

static int nfail;
static int npass;

#define TEST(name) do { \
    printf("[TEST] %s ... ", name); \
    fflush(stdout); \
} while(0)
#define PASS() do { npass++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { nfail++; printf("FAIL: %s\n", msg); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ===== 1. kmem stress ===== */
static void test_kmem_stress(void) {
    TEST("kmem malloc/free all classes");
    static const int sizes[] = {8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1280, 1536, 2048};
    enum { NSIZES = sizeof(sizes) / sizeof(sizes[0]) };
    void *ptrs[NSIZES];

    for (int i = 0; i < NSIZES; i++) {
        ptrs[i] = malloc(sizes[i]);
        if (!ptrs[i]) { FAIL("malloc"); return; }
        memset(ptrs[i], 0xAB, sizes[i]);
    }
    for (int i = 0; i < NSIZES; i++) {
        unsigned char *p = ptrs[i];
        for (int j = 0; j < sizes[i]; j++) {
            if (p[j] != 0xAB) { FAIL("corruption"); return; }
        }
    }
    for (int i = 0; i < NSIZES; i++) free(ptrs[i]);

    for (int rep = 0; rep < 200; rep++) {
        int idx = rand() % NSIZES;
        void *p = malloc(sizes[idx]);
        if (!p) { FAIL("malloc"); return; }
        memset(p, rep, sizes[idx]);
        free(p);
    }
    PASS();
}

static void test_kmem_realloc(void) {
    TEST("kmem realloc pattern");
    void *p = malloc(48);
    ASSERT(p, "malloc(48)");
    memset(p, 0x11, 48);

    void *q = realloc(p, 96);
    ASSERT(q, "realloc 48->96");
    unsigned char *cq = q;
    for (int i = 0; i < 48; i++) ASSERT(cq[i] == 0x11, "realloc data lost");
    memset(q, 0x22, 96);

    q = realloc(q, 32);
    ASSERT(q, "realloc 96->32");
    cq = q;
    for (int i = 0; i < 32; i++) ASSERT(cq[i] == 0x22, "realloc shrink data lost");
    free(q);
    PASS();
}

/* ===== 2. Pipe ===== */
static void test_pipe_large_rw(void) {
    TEST("pipe large data (4096B)");
    int p[2];
    ASSERT(pipe(p) == 0, "pipe");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");

    if (pid == 0) {
        close(p[0]);
        char buf[4096];
        for (int i = 0; i < 64; i++) {
            memset(buf, (char)i, sizeof(buf));
            int w = write(p[1], buf, sizeof(buf));
            ASSERT(w == sizeof(buf), "write");
        }
        close(p[1]);
        _exit(0);
    }

    close(p[1]);
    char buf[4096];
    int total = 0;
    int r;
    while ((r = read(p[0], buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r; i++)
            ASSERT(buf[i] == (char)(total / 4096), "pipe data integrity");
        total += r;
    }
    ASSERT(total == 64 * 4096, "pipe total bytes");
    close(p[0]);

    int ws;
    waitpid(pid, &ws, 0);
    ASSERT(ws == 0, "child exit");
    PASS();
}

static void test_pipe_select(void) {
    TEST("pipe select POLLIN (cross-check scenario)");
    int p[2];
    ASSERT(pipe(p) == 0, "pipe");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");

    if (pid == 0) {
        close(p[0]);
        usleep(1000);
        uint8_t v = 0x42;
        write(p[1], &v, 1);
        close(p[1]);
        _exit(0);
    }

    close(p[1]);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

    int r = select(p[0] + 1, &rfds, NULL, NULL, &tv);
    ASSERT(r > 0, "select should not timeout");
    ASSERT(FD_ISSET(p[0], &rfds), "FD_ISSET");

    uint8_t v;
    ASSERT(read(p[0], &v, 1) == 1, "read 1 byte");
    ASSERT(v == 0x42, "correct value");
    close(p[0]);

    int ws;
    waitpid(pid, &ws, 0);
    ASSERT(ws == 0, "child exit");
    PASS();
}

/* ===== 3. SCM_RIGHTS ===== */
static int send_fds(int socket, int *fds, int nfds) {
    struct msghdr msg = {0};
    size_t cmsg_space = CMSG_SPACE(sizeof(int) * nfds);
    char *ctl = malloc(cmsg_space);
    if (!ctl) return -1;
    memset(ctl, 0, cmsg_space);

    struct iovec io = { .iov_base = "M", .iov_len = 1 };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = cmsg_space;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);

    int ret = (int)sendmsg(socket, &msg, 0);
    free(ctl);
    return ret;
}

static int recv_fds(int socket, int *fds, int maxfds) {
    struct msghdr msg = {0};
    size_t cmsg_space = CMSG_SPACE(sizeof(int) * maxfds);
    char *ctl = malloc(cmsg_space);
    if (!ctl) return -1;
    memset(ctl, 0, cmsg_space);

    char dummy;
    struct iovec io = { .iov_base = &dummy, .iov_len = 1 };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = cmsg_space;

    ssize_t n = recvmsg(socket, &msg, 0);
    if (n <= 0) { free(ctl); return -1; }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    int got = 0;
    while (cmsg) {
        if (cmsg->cmsg_type == SCM_RIGHTS) {
            int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            if (count > maxfds) count = maxfds;
            memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * count);
            got = count;
        }
        cmsg = CMSG_NXTHDR(&msg, cmsg);
    }
    free(ctl);
    return got;
}

static void test_scm_multi_fd(void) {
    TEST("SCM_RIGHTS multi-fd");
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");

    if (pid == 0) {
        close(sv[0]);
        int fds[3];
        for (int i = 0; i < 3; i++) {
            fds[i] = memfd_create("mfd", 0);
            ASSERT(fds[i] >= 0, "memfd");
            ftruncate(fds[i], 4096);
        }
        int r = send_fds(sv[1], fds, 3);
        ASSERT(r > 0, "sendmsg");
        for (int i = 0; i < 3; i++) close(fds[i]);
        close(sv[1]);
        _exit(0);
    }

    close(sv[1]);
    int fds[3];
    int got = recv_fds(sv[0], fds, 3);
    ASSERT(got == 3, "recv 3 fds");

    for (int i = 0; i < 3; i++) {
        ASSERT(fcntl(fds[i], F_GETFD) != -1, "fd valid");
        close(fds[i]);
    }
    close(sv[0]);

    int ws;
    waitpid(pid, &ws, 0);
    ASSERT(ws == 0, "child exit");
    PASS();
}

/* ===== 4. Epoll ET ===== */
static void test_epoll_et(void) {
    TEST("epoll edge-triggered + pipe");
    int p[2];
    ASSERT(pipe(p) == 0, "pipe");

    int epfd = epoll_create1(0);
    ASSERT(epfd >= 0, "epoll_create");

    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = p[0] };
    ASSERT(epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) == 0, "epoll_ctl_add");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");

    if (pid == 0) {
        close(p[0]);
        write(p[1], "A", 1);
        usleep(2000);
        write(p[1], "B", 1);
        close(p[1]);
        _exit(0);
    }

    close(p[1]);
    struct epoll_event evs[4];

    /* First EPOLLET wake: should get 'A' */
    int n = epoll_wait(epfd, evs, 4, 5000);
    ASSERT(n == 1, "first ET wake");
    ASSERT(evs[0].events & EPOLLIN, "EPOLLIN");
    char c;
    read(p[0], &c, 1);
    ASSERT(c == 'A', "first byte");

    /* Drain all remaining data (ET needs drain) */
    while (read(p[0], &c, 1) == 1)
        ;

    /* Wait for 'B' — ET should fire again */
    n = epoll_wait(epfd, evs, 4, 5000);
    ASSERT(n == 1, "second ET wake");
    read(p[0], &c, 1);
    ASSERT(c == 'B', "second byte");

    close(p[0]);
    close(epfd);

    int ws;
    waitpid(pid, &ws, 0);
    ASSERT(ws == 0, "child exit");
    PASS();
}

static void test_epoll_oneshot(void) {
    TEST("epoll oneshot + rearm");
    int p[2];
    ASSERT(pipe(p) == 0, "pipe");

    int epfd = epoll_create1(0);
    ASSERT(epfd >= 0, "epoll_create");

    struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT, .data.fd = p[0] };
    ASSERT(epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) == 0, "epoll_ctl_add");

    write(p[1], "X", 1);

    struct epoll_event evs[2];
    int n = epoll_wait(epfd, evs, 2, 5000);
    ASSERT(n == 1, "oneshot fire");

    char c;
    read(p[0], &c, 1);

    /* Second write — should NOT fire (oneshot disarmed) */
    write(p[1], "Y", 1);
    n = epoll_wait(epfd, evs, 2, 200);
    ASSERT(n == 0, "oneshot disarmed");

    /* Rearm */
    ev.events = EPOLLIN | EPOLLONESHOT;
    ASSERT(epoll_ctl(epfd, EPOLL_CTL_MOD, p[0], &ev) == 0, "epoll_ctl_mod");

    n = epoll_wait(epfd, evs, 2, 5000);
    ASSERT(n == 1, "rearmed fire");
    read(p[0], &c, 1);
    ASSERT(c == 'Y', "rearmed data");

    close(p[0]);
    close(p[1]);
    close(epfd);
    PASS();
}

/* ===== 5. Futex ===== */
static void test_futex_wake(void) {
    TEST("futex basic wait/wake");
    volatile uint32_t *futex = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT(futex != MAP_FAILED, "mmap");
    *futex = 0;

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");

    if (pid == 0) {
        int r = syscall(SYS_futex, futex, FUTEX_WAIT, 0, NULL, NULL, 0);
        ASSERT(r == 0 || errno == EAGAIN, "futex wait");
        ASSERT(*futex == 1, "futex value after wake");
        munmap((void *)futex, 4096);
        _exit(0);
    }

    usleep(50000);
    *futex = 1;
    int r = syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 0);
    ASSERT(r > 0, "futex wake");

    int ws;
    waitpid(pid, &ws, 0);
    ASSERT(ws == 0, "child exit");
    munmap((void *)futex, 4096);
    PASS();
}

static void test_futex_timeout(void) {
    TEST("futex wait timeout");
    volatile uint32_t futex_val = 0;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
    int r = syscall(SYS_futex, (uint32_t *)&futex_val, FUTEX_WAIT, 0, &ts, NULL, 0);
    ASSERT(r == -1 && errno == ETIMEDOUT, "futex wait timeout");
    PASS();
}

/* ===== 7. Filesystem: create / write / read / unlink ===== */
static void test_fs_file_ops(void) {
    TEST("fs create/write/read/unlink");
    const char *path = "/test_fs_ops.dat";
    uint8_t pattern[256];
    for (int i = 0; i < 256; i++) pattern[i] = (uint8_t)i;

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    ASSERT(fd >= 0, "open(O_CREAT)");
    ASSERT(write(fd, pattern, 256) == 256, "write 256B");
    off_t pos = lseek(fd, 0, SEEK_CUR);
    ASSERT(pos == 256, "lseek SEEK_CUR");

    ASSERT(lseek(fd, 128, SEEK_SET) == 128, "lseek SEEK_SET");
    uint8_t buf[256];
    ASSERT(read(fd, buf, 64) == 64, "read 64B");
    for (int i = 0; i < 64; i++) ASSERT(buf[i] == pattern[128 + i], "read data");

    ASSERT(lseek(fd, 0, SEEK_SET) == 0, "lseek rewind");
    ASSERT(read(fd, buf, 256) == 256, "read full");
    for (int i = 0; i < 256; i++) ASSERT(buf[i] == pattern[i], "full data");
    close(fd);

    ASSERT(access(path, F_OK) == 0, "access OK");
    ASSERT(unlink(path) == 0, "unlink");
    ASSERT(access(path, F_OK) == -1, "access after unlink");
    PASS();
}

/* ===== 8. Filesystem: stat / fstat ===== */
static void test_fs_stat(void) {
    TEST("fs stat/fstat/lstat");
    struct stat st;

    ASSERT(stat("/dev/tty", &st) == 0, "stat /dev/tty");
    ASSERT(S_ISCHR(st.st_mode), "is chardev");

    ASSERT(lstat("/dev/tty", &st) == 0, "lstat /dev/tty");
    ASSERT(S_ISCHR(st.st_mode), "lstat chardev");

    int fd = open("/dev/tty", O_RDONLY);
    ASSERT(fd >= 0, "open /dev/tty");
    ASSERT(fstat(fd, &st) == 0, "fstat");
    ASSERT(S_ISCHR(st.st_mode), "fstat chardev");
    ASSERT(st.st_size >= 0, "fstat size");
    close(fd);

    ASSERT(stat("/nonexistent", &st) == -1, "stat nonexistent");
    ASSERT(errno == ENOENT, "errno ENOENT");
    PASS();
}

/* ===== 9. Filesystem: mkdir / getdents / rmdir ===== */
static void test_fs_mkdir_getdents(void) {
    TEST("fs mkdir/getdents/rmdir");
    ASSERT(mkdir("/test_dir", 0755) == 0, "mkdir");
    struct stat st;
    ASSERT(stat("/test_dir", &st) == 0, "stat new dir");
    ASSERT(S_ISDIR(st.st_mode), "is dir");

    DIR *    d = opendir("/test_dir");
    ASSERT(d != NULL, "opendir");
    { struct dirent *de; while ((de = readdir(d)) != NULL) { (void)de; } }
    closedir(d);

    int fd = open("/test_dir/file", O_CREAT | O_RDWR, 0644);
    ASSERT(fd >= 0, "create file in dir");
    close(fd);

    d = opendir("/test_dir");
    ASSERT(d != NULL, "opendir2");
    int found = 0;
    { struct dirent *de; while ((de = readdir(d)) != NULL) { if (strcmp(de->d_name, "file") == 0) found = 1; } }
    ASSERT(found, "found 'file' entry");
    closedir(d);

    ASSERT(unlink("/test_dir/file") == 0, "unlink file");
    ASSERT(rmdir("/test_dir") == 0, "rmdir");
    ASSERT(stat("/test_dir", &st) == -1, "stat removed dir");
    PASS();
}

/* ===== 10. Signals: handler registration and delivery ===== */
static volatile sig_atomic_t sig_flag = 0;
static volatile int sig_received = 0;

static void sig_usr1_handler(int signo) {
    sig_received = signo;
    sig_flag = 1;
}

static void test_signal_handler(void) {
    TEST("signal handler delivery");
    sig_flag = 0;
    sig_received = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_usr1_handler;
    sigemptyset(&sa.sa_mask);
    ASSERT(sigaction(SIGUSR1, &sa, NULL) == 0, "sigaction");

    kill(getpid(), SIGUSR1);
    ASSERT(sig_flag == 1, "handler was called");
    ASSERT(sig_received == SIGUSR1, "correct signo");
    PASS();
}

/* ===== 11. Signals: SA_SIGINFO and chained handlers ===== */
static volatile int sig_chain[4];
static volatile int sig_chain_idx;

static void sig_chain_handler_1(int signo) {
    (void)signo;
    sig_chain[sig_chain_idx++] = 1;
}

static void sig_chain_handler_2(int signo) {
    (void)signo;
    sig_chain[sig_chain_idx++] = 2;
}

static void test_signal_sigaction(void) {
    TEST("signal sigaction replace/restore");
    struct sigaction sa, old;

    sig_chain_idx = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_chain_handler_1;
    sigemptyset(&sa.sa_mask);
    ASSERT(sigaction(SIGUSR2, &sa, &old) == 0, "sigaction set");
    kill(getpid(), SIGUSR2);
    ASSERT(sig_chain[0] == 1, "handler1 called");

    sa.sa_handler = sig_chain_handler_2;
    ASSERT(sigaction(SIGUSR2, &sa, &old) == 0, "sigaction replace");
    kill(getpid(), SIGUSR2);
    ASSERT(sig_chain[1] == 2, "handler2 called");

    ASSERT(sigaction(SIGUSR2, &old, NULL) == 0, "sigaction restore");
    kill(getpid(), SIGUSR2);
    ASSERT(sig_chain[2] == 1, "restored handler called");
    PASS();
}

/* ===== 12. mprotect ===== */
static void test_mmap_prot(void) {
    TEST("mprotect PROT_READ/PROT_WRITE");
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT(p != MAP_FAILED, "mmap");
    *(volatile int *)p = 0x1234;
    ASSERT(*(volatile int *)p == 0x1234, "write+read");

    ASSERT(mprotect(p, 4096, PROT_READ) == 0, "mprotect PROT_READ");
    ASSERT(*(volatile int *)p == 0x1234, "read after PROT_READ");

    ASSERT(mprotect(p, 4096, PROT_READ | PROT_WRITE) == 0, "mprotect RW");
    *(volatile int *)p = 0x5678;
    ASSERT(*(volatile int *)p == 0x5678, "write after restore");

    ASSERT(mprotect((void *)((uintptr_t)p + 2048), 2048, PROT_READ) == 0,
           "mprotect partial page range");

    ASSERT(munmap(p, 4096) == 0, "munmap");
    PASS();
}

/* ===== 13. mremap ===== */
static void test_mremap(void) {
    TEST("mremap expand");
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ASSERT(p != MAP_FAILED, "mmap 4K");
    memset(p, 0xAB, 4096);

    void *q = mremap(p, 4096, 16384, MREMAP_MAYMOVE);
    ASSERT(q != MAP_FAILED, "mremap 4K->16K");
    unsigned char *cq = (unsigned char *)q;
    for (int i = 0; i < 4096; i++) ASSERT(cq[i] == 0xAB, "data after expand");
    memset(q, 0xCD, 16384);
    ASSERT(munmap(q, 16384) == 0, "munmap");
    PASS();
}

/* ===== 14. poll ===== */
static void test_poll_basic(void) {
    TEST("poll on pipe");
    int p[2];
    ASSERT(pipe(p) == 0, "pipe");

    struct pollfd fds[1];
    fds[0].fd = p[0];
    fds[0].events = POLLIN;

    ASSERT(poll(fds, 1, 10) == 0, "poll timeout (empty)");

    write(p[1], "X", 1);
    int n = poll(fds, 1, 5000);
    ASSERT(n == 1, "poll readable");
    ASSERT(fds[0].revents & POLLIN, "POLLIN flag");

    char c;
    read(p[0], &c, 1);
    ASSERT(c == 'X', "data match");
    close(p[0]);
    close(p[1]);
    PASS();
}

/* ===== 15. eventfd ===== */
static void test_eventfd(void) {
    TEST("eventfd read/write + semaphore");
    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT(efd >= 0, "eventfd create");

    eventfd_t val;
    ASSERT(read(efd, &val, sizeof(val)) == -1, "read empty (EAGAIN)");
    ASSERT(errno == EAGAIN, "errno EAGAIN");

    ASSERT(eventfd_write(efd, 3) == 0, "write 3");
    ASSERT(eventfd_read(efd, &val) == 0, "read");
    ASSERT(val == 3, "value == 3");

    ASSERT(eventfd_write(efd, (eventfd_t)-5) == 0, "write large");
    ASSERT(eventfd_read(efd, &val) == 0, "read large");
    ASSERT(val == (eventfd_t)-5, "large value preserved");
    close(efd);

    int sefd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    ASSERT(sefd >= 0, "semaphore eventfd");
    ASSERT(eventfd_write(sefd, 2) == 0, "sem write 2");
    ASSERT(eventfd_read(sefd, &val) == 0, "sem read #1");
    ASSERT(val == 1, "semaphore: read 1");
    ASSERT(eventfd_read(sefd, &val) == 0, "sem read #2");
    ASSERT(val == 1, "semaphore: read 1 again");
    ASSERT(eventfd_read(sefd, &val) == -1, "sem read empty");
    ASSERT(errno == EAGAIN, "sem empty EAGAIN");
    close(sefd);
    PASS();
}

/* ===== 16. clock_gettime sanity ===== */
static void test_clock_gettime(void) {
    TEST("clock_gettime monotonic");
    struct timespec ts1, ts2;
    ASSERT(clock_gettime(CLOCK_MONOTONIC, &ts1) == 0, "gettime #1");
    usleep(1000);
    ASSERT(clock_gettime(CLOCK_MONOTONIC, &ts2) == 0, "gettime #2");

    int64_t ns1 = (int64_t)ts1.tv_sec * 1000000000LL + ts1.tv_nsec;
    int64_t ns2 = (int64_t)ts2.tv_sec * 1000000000LL + ts2.tv_nsec;
    ASSERT(ns2 > ns1, "time advanced");

    ASSERT(ts1.tv_nsec >= 0 && ts1.tv_nsec < 1000000000L, "tv_nsec range");
    PASS();
}

/* ===== 17. Multi-threaded stress ===== */
#include <pthread.h>

static volatile size_t last_stdout_value;
static void check_stdout(const char *where) {
    size_t v = (size_t)stdout;
    if (v == 0xb30e8 || v == 0) {
        printf("[STDOUT-CHECK] %s: stdout=BROKEN (%p)\n", where, stdout);
    }
    last_stdout_value = v;
}
int send_fd(int socket, int fd_to_send);
int recv_fd(int socket);

#define MT_NUM 6
#define MT_ITER 200

static int mt_pipes[MT_NUM][2];
static int mt_socks[MT_NUM][2];
static volatile uint32_t mt_barrier;
static volatile int mt_fail;

struct mt_arg {
    int id;
};

static void mt_barrier_wait(void) {
    __sync_fetch_and_add(&mt_barrier, 1);
    if (mt_barrier == MT_NUM) {
        mt_barrier = 0;
        syscall(SYS_futex, (uint32_t *)&mt_barrier, FUTEX_WAKE, MT_NUM, NULL, NULL, 0);
    } else {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        syscall(SYS_futex, (uint32_t *)&mt_barrier, FUTEX_WAIT, 0, &ts, NULL, 0);
    }
}

static void *mt_worker(void *arg) {
    int id = ((struct mt_arg *)arg)->id;

    for (int iter = 0; iter < MT_ITER; iter++) {
        int val = id ^ (iter + 1);

        /* Pipe: write val, read back */
        write(mt_pipes[id][1], &val, sizeof(val));
        int rval = -1;
        read(mt_pipes[id][0], &rval, sizeof(rval));
        if (rval != val) { mt_fail = 1; return NULL; }

        /* SCM_RIGHTS: send fd, recv fd, verify data */
        int memfd = memfd_create("mt", 0);
        if (memfd < 0) { mt_fail = 1; return NULL; }
        ftruncate(memfd, 4096);
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (p == MAP_FAILED) { mt_fail = 1; close(memfd); return NULL; }
        memset(p, val, 4096);
        munmap(p, 4096);
        if (send_fd(mt_socks[id][1], memfd) < 0) { mt_fail = 1; close(memfd); return NULL; }
        close(memfd);

        int rfd = recv_fd(mt_socks[id][0]);
        if (rfd < 0) { mt_fail = 1; return NULL; }
        void *rp = mmap(NULL, 4096, PROT_READ, MAP_SHARED, rfd, 0);
        if (rp == MAP_FAILED) { mt_fail = 1; close(rfd); return NULL; }
        unsigned char *cp = rp;
        for (int i = 0; i < 4096; i++) {
            if (cp[i] != (unsigned char)val) { mt_fail = 1; break; }
        }
        munmap(rp, 4096);
        close(rfd);
        if (mt_fail) return NULL;

        /* kmem: concurrent malloc/free */
        for (int i = 0; i < 30; i++) {
            size_t sz = 8 + (iter * 7 + i * 13) % 1024;
            void *q = malloc(sz);
            if (!q) { mt_fail = 1; return NULL; }
            memset(q, val ^ i, sz);
            free(q);
        }

        /* mprotect: PROT_READ / PROT_WRITE toggle */
        void *mp = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (mp != MAP_FAILED) {
            *(volatile int *)mp = val;
            if (*(volatile int *)mp != val) mt_fail = 1;
            mprotect(mp, 4096, PROT_READ);
            if (*(volatile int *)mp != val) mt_fail = 1;
            mprotect(mp, 4096, PROT_READ | PROT_WRITE);
            *(volatile int *)mp = val + 1;
            if (*(volatile int *)mp != val + 1) mt_fail = 1;
            munmap(mp, 4096);
        }
        if (mt_fail) return NULL;
    }

    mt_barrier_wait();
    return NULL;
}

static void test_multithreaded(void) {
    TEST("multi-threaded stress (6 threads, 200 iters)");

    for (int i = 0; i < MT_NUM; i++) {
        ASSERT(pipe(mt_pipes[i]) == 0, "mt pipe");
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, mt_socks[i]) == 0, "mt socketpair");
    }

    mt_barrier = 0;
    mt_fail = 0;
    pthread_t threads[MT_NUM];
    struct mt_arg args[MT_NUM];
    for (int i = 0; i < MT_NUM; i++) {
        args[i].id = i;
        { int _perr = pthread_create(&threads[i], NULL, mt_worker, &args[i]); printf("  [pthread %d] err=%d errno=%d\n", i, _perr, errno); ASSERT(_perr == 0, "pthread_create"); }
    }

    for (int i = 0; i < MT_NUM; i++) {
        pthread_join(threads[i], NULL);
    }

    ASSERT(!mt_fail, "no thread failures");

    for (int i = 0; i < MT_NUM; i++) {
        close(mt_pipes[i][0]); close(mt_pipes[i][1]);
        close(mt_socks[i][0]); close(mt_socks[i][1]);
    }
    PASS();
}

/* ===== 18. Concurrent VM ops (mmap/mprotect battle) ===== */
#define VM_MT_NUM 4

static volatile int vm_mt_fail;
static volatile uint32_t vm_barrier;

struct vm_mt_arg {
    int id;
    volatile int *shared_val;
};

static void vm_barrier_wait(void) {
    __sync_fetch_and_add(&vm_barrier, 1);
    if (vm_barrier == VM_MT_NUM) {
        vm_barrier = 0;
        syscall(SYS_futex, (uint32_t *)&vm_barrier, FUTEX_WAKE, VM_MT_NUM, NULL, NULL, 0);
    } else {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        syscall(SYS_futex, (uint32_t *)&vm_barrier, FUTEX_WAIT, 0, &ts, NULL, 0);
    }
}

static void *vm_worker(void *arg) {
    struct vm_mt_arg *a = arg;
    int id = a->id;

    for (int iter = 0; iter < 200; iter++) {
        int val = id ^ (iter + 1);

        /* mmap + mprotect toggling */
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) { vm_mt_fail = 1; return NULL; }

        for (int r = 0; r < 5; r++) {
            *(volatile int *)p = val;
            mprotect(p, 4096, PROT_READ);
            if (*(volatile int *)p != val) { vm_mt_fail = 1; break; }
            mprotect(p, 4096, PROT_READ | PROT_WRITE);
            *(volatile int *)p = val + 1;
        }

        munmap(p, 4096);

        /* Anonymous MAP_SHARED via shm */
        int shm = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
        if (shm < 0) { vm_mt_fail = 1; return NULL; }
        void *sh = shmat(shm, NULL, 0);
        if (sh == (void *)-1) { vm_mt_fail = 1; return NULL; }
        *(volatile int *)sh = val;
        if (*(volatile int *)sh != val) { vm_mt_fail = 1; }
        shmdt(sh);
        shmctl(shm, IPC_RMID, NULL);

        if (vm_mt_fail) return NULL;
    }

    vm_barrier_wait();
    return NULL;
}

static void test_mt_concurrent_vm(void) {
    TEST("concurrent VM: mmap/mprotect/shm (4 threads, 200 iters)");

    vm_barrier = 0;
    vm_mt_fail = 0;
    pthread_t threads[VM_MT_NUM];
    struct vm_mt_arg args[VM_MT_NUM];
    for (int i = 0; i < VM_MT_NUM; i++) {
        args[i].id = i;
        { int _perr = pthread_create(&threads[i], NULL, vm_worker, &args[i]); printf("  [vm %d] err=%d errno=%d\n", i, _perr, errno); ASSERT(_perr == 0, "vm pthread_create"); }
    }
    for (int i = 0; i < VM_MT_NUM; i++) {
        pthread_join(threads[i], NULL);
    }
    ASSERT(!vm_mt_fail, "no vm thread failures");
    PASS();
}

/* ===== 19. Concurrent epoll ===== */
#define EP_MT_NUM 4
#define EP_MT_PIPES 8

static volatile int ep_mt_fail;
static volatile uint32_t ep_barrier;

struct ep_mt_arg {
    int id;
    int epfd;
    int pipes[EP_MT_PIPES][2];
};

static void ep_barrier_wait(void) {
    __sync_fetch_and_add(&ep_barrier, 1);
    if (ep_barrier == EP_MT_NUM) {
        ep_barrier = 0;
        syscall(SYS_futex, (uint32_t *)&ep_barrier, FUTEX_WAKE, EP_MT_NUM, NULL, NULL, 0);
    } else {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        syscall(SYS_futex, (uint32_t *)&ep_barrier, FUTEX_WAIT, 0, &ts, NULL, 0);
    }
}

static void *ep_worker(void *arg) {
    struct ep_mt_arg *a = arg;
    int epfd = a->epfd;

    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < EP_MT_PIPES; i++) {
            struct epoll_event ev = {
                .events = EPOLLIN | EPOLLOUT | (iter % 2 ? EPOLLET : 0),
                .data.fd = a->pipes[i][0],
            };
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, a->pipes[i][0], &ev) < 0 &&
                errno != EEXIST) {
                ep_mt_fail = 1; return NULL;
            }

            ev.events = EPOLLIN;
            if (epoll_ctl(epfd, EPOLL_CTL_MOD, a->pipes[i][0], &ev) < 0) {
                ep_mt_fail = 1; return NULL;
            }
        }

        int nfds = epoll_wait(epfd, (struct epoll_event[16]){0}, 16, 0);
        if (nfds < 0) { ep_mt_fail = 1; return NULL; }

        for (int i = 0; i < EP_MT_PIPES; i++) {
            if (epoll_ctl(epfd, EPOLL_CTL_DEL, a->pipes[i][0], NULL) < 0) {
                ep_mt_fail = 1; return NULL;
            }
        }
    }

    ep_barrier_wait();
    return NULL;
}

static void test_mt_concurrent_epoll(void) {
    TEST("concurrent epoll add/mod/del (4 threads, 8 pipes, 100 iters)");

    int epfd = epoll_create1(0);
    printf("  [ep] epfd=%d\n", epfd); fflush(stdout);
    ASSERT(epfd >= 0, "epoll_create1");

    ep_barrier = 0;
    ep_mt_fail = 0;
    pthread_t threads[EP_MT_NUM];
    struct ep_mt_arg args[EP_MT_NUM];
    for (int i = 0; i < EP_MT_NUM; i++) {
        args[i].id = i;
        args[i].epfd = epfd;
        for (int j = 0; j < EP_MT_PIPES; j++) {
            ASSERT(pipe(args[i].pipes[j]) == 0, "ep pipe");
        }
        printf("  [ep] creating thread %d...\n", i); fflush(stdout);
        ASSERT(pthread_create(&threads[i], NULL, ep_worker, &args[i]) == 0, "ep pthread_create");
        printf("  [ep] thread %d created\n", i); fflush(stdout);
    }

    for (int i = 0; i < EP_MT_NUM; i++) {
        pthread_join(threads[i], NULL);
        for (int j = 0; j < EP_MT_PIPES; j++) {
            close(args[i].pipes[j][0]); close(args[i].pipes[j][1]);
        }
    }
    close(epfd);
    ASSERT(!ep_mt_fail, "no epoll thread failures");
    PASS();
}

/* ===== 20. Original cross-check stress test ===== */
static int count_my_fds(void) {
    int cnt = 0;
    for (int i = 0; i < 256; i++) {
        if (fcntl(i, F_GETFD) != -1) cnt++;
    }
    return cnt;
}

static void pressure_malloc(void) {
    for (int i = 0; i < 64; i++) {
        volatile char *p = malloc(65536);
        if (p) {
            p[0] = (char)i;
            p[65535] = (char)~i;
        }
    }
}

int send_fd(int socket, int fd_to_send) {
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = { .iov_base = "C", .iov_len = 1 };

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd_to_send;

    return sendmsg(socket, &msg, 0);
}

int recv_fd(int socket) {
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io;
    char dummy;

    io.iov_base = &dummy;
    io.iov_len = 1;
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    ssize_t n = recvmsg(socket, &msg, 0);
    if (n <= 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        return *((int *)CMSG_DATA(cmsg));
    }
    return -1;
}

void child_client(int write_sock, int child_id, int cross_pipe_fd) {
    char buf[64];
    int n;

    for (int i = 0; i < ITERATIONS; i++) {
        int mem_fd = memfd_create("stress_shm", 0);
        if (mem_fd < 0) { perror("memfd_create"); exit(1); }
        if (ftruncate(mem_fd, SHM_SIZE) < 0) { perror("ftruncate"); exit(1); }

        void *ptr = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED, mem_fd, 0);
        if (ptr == MAP_FAILED) { perror("mmap"); exit(1); }

        uint8_t start_byte = 0xAA ^ (uint8_t)(i & 0xFF);
        uint8_t end_byte   = 0xBB ^ (uint8_t)((i >> 4) & 0xFF);
        memset(ptr, start_byte, 4);
        memset((char*)ptr + SHM_SIZE - 4, end_byte, 4);

        munmap(ptr, SHM_SIZE);
        ptr = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, mem_fd, 0);
        if (ptr == MAP_FAILED) { perror("mmap2"); exit(1); }

        memset(ptr, start_byte, 4);
        memset((char*)ptr + SHM_SIZE - 4, end_byte, 4);

        if (send_fd(write_sock, mem_fd) < 0) { perror("send_fd"); exit(1); }

        munmap(ptr, SHM_SIZE);
        close(mem_fd);

        if (i % 50 == 49) pressure_malloc();
        if (i % 50 == 0) usleep(5);
    }

    uint8_t done = (uint8_t)child_id;
    write(cross_pipe_fd, &done, 1);

    close(write_sock);
    close(cross_pipe_fd);
}

void parent_compositor(int read_socks[NUM_CHILDREN]) {
    int epfd = epoll_create1(0);
    struct epoll_event ev, events[NUM_CHILDREN];

    for (int i = 0; i < NUM_CHILDREN; i++) {
        ev.events = EPOLLIN;
        ev.data.fd = read_socks[i];
        epoll_ctl(epfd, EPOLL_CTL_ADD, read_socks[i], &ev);
    }

    int total_verifications = NUM_CHILDREN * ITERATIONS;
    int success_count = 0;
    time_t start = time(NULL);

    printf("[Parent] Epoll loop start (%d total).\n", total_verifications);

    int child_iter[NUM_CHILDREN];
    memset(child_iter, 0, sizeof(child_iter));

    while (success_count < total_verifications) {
        if (time(NULL) - start > TIMEOUT_SEC) {
            printf("[FAIL] Parent TIMEOUT! (%d/%d verified)\n",
                   success_count, total_verifications);
            break;
        }

        int nfds = epoll_wait(epfd, events, NUM_CHILDREN, 5000);
        if (nfds < 0) { perror("epoll_wait"); break; }
        if (nfds == 0) continue;

        for (int i = 0; i < nfds; i++) {
            int sock = events[i].data.fd;

            if (events[i].events & EPOLLIN) {
                int rfd = recv_fd(sock);
                if (rfd < 0) continue;

                void *ptr = mmap(NULL, SHM_SIZE, PROT_READ,
                                 MAP_SHARED, rfd, 0);
                if (ptr == MAP_FAILED) {
                    perror("Parent mmap");
                    close(rfd);
                    continue;
                }

                int child_idx = -1;
                for (int c = 0; c < NUM_CHILDREN; c++) {
                    if (read_socks[c] == sock) { child_idx = c; break; }
                }

                int iter = (child_idx >= 0) ? child_iter[child_idx]++ : 0;
                uint8_t expected_start = 0xAA ^ (uint8_t)(iter & 0xFF);
                uint8_t expected_end   = 0xBB ^ (uint8_t)((iter >> 4) & 0xFF);

                unsigned char *cptr = (unsigned char *)ptr;
                if (cptr[0] == expected_start && cptr[SHM_SIZE - 1] == expected_end) {
                    success_count++;
                } else {
                    printf("Data corruption! child=%d iter=%d "
                           "got 0x%02x/0x%02x expected 0x%02x/0x%02x\n",
                           child_idx, iter,
                           cptr[0], cptr[SHM_SIZE - 1],
                           expected_start, expected_end);
                }

                munmap(ptr, SHM_SIZE);
                close(rfd);
            }

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, sock, NULL);
            }
        }
    }

    printf("[Parent] Verification Complete. %d / %d clean.\n",
           success_count, total_verifications);
    close(epfd);
}

static void test_disk_rw(void) {
    TEST("disk read/write via /dev/sda");

    int fd = open("/dev/sda", O_RDWR);
    if (fd < 0) { FAIL("open /dev/sda"); return; }

    uint8_t buf[512] __attribute__((aligned(512)));
    uint8_t wbuf[512] __attribute__((aligned(512)));

    for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i & 0xFF);
    if (lseek(fd, 512, SEEK_SET) != 512) { FAIL("lseek before write"); goto out; }
    ssize_t n = write(fd, wbuf, sizeof(wbuf));
    if (n != 512) { FAIL("write sector 1"); goto out; }

    __builtin_memset(buf, 0, sizeof(buf));
    if (lseek(fd, 512, SEEK_SET) != 512) { FAIL("lseek before readback"); goto out; }
    n = read(fd, buf, sizeof(buf));
    if (n != 512) { FAIL("readback size"); goto out; }
    if (__builtin_memcmp(buf, wbuf, 512) != 0) { FAIL("data mismatch"); goto out; }

    PASS();
out:
    close(fd);
}

static void test_ext2_read(void) {
    TEST("ext2 filesystem read via /mnt/testfile.txt");

    int fd = open("/mnt/testfile.txt", O_RDONLY);
    if (fd < 0) { FAIL("open /mnt/testfile.txt"); return; }

    char buf[64];
    __builtin_memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { FAIL("read"); close(fd); return; }
    buf[n] = '\0';

    // Check prefix (15 chars: "Hello from ext2")
    if (__builtin_memcmp(buf, "Hello from ext2", 15) != 0) {
        FAIL("content mismatch");
        printf("  Got: %s\n", buf);
        printf("  Hex buf:      ");
        for (int i = 0; i < 20 && i < n; i++)
            printf("%02x ", (unsigned char)buf[i]);
        printf("\n");
        close(fd);
        return;
    }

    close(fd);
    PASS();
}

static void test_ext2_write(void) {
    TEST("ext2 create/write/read on ext2");

    int fd = open("/mnt/ext2_test_write.dat", O_CREAT | O_RDWR, 0644);
    if (fd < 0) { FAIL("open /mnt/ext2_test_write.dat"); return; }
    printf("  opened fd=%d\n", fd);

    const char *msg = "Hello from ext2 write!";
    size_t len = strlen(msg);
    printf("  calling write, len=%zu\n", len);
    fflush(stdout);
    ssize_t n = write(fd, msg, len);
    printf("  write returned %zd\n", n);
    if (n != (ssize_t)len) { FAIL("write"); close(fd); unlink("/mnt/ext2_test_write.dat"); return; }
    printf("  write ok, calling lseek\n");
    fflush(stdout);

    lseek(fd, 0, SEEK_SET);

    char buf[64];
    __builtin_memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { FAIL("read"); close(fd); unlink("/mnt/ext2_test_write.dat"); return; }
    buf[n] = '\0';

    if (strcmp(buf, msg) != 0) {
        FAIL("content mismatch");
        printf("  Expected: %s\n", msg);
        printf("  Got:      %s\n", buf);
        close(fd);
        unlink("/mnt/ext2_test_write.dat");
        return;
    }

    close(fd);
    unlink("/mnt/ext2_test_write.dat");
    PASS();
}

/* ===== SMP ext2 concurrent read/write ===== */
#define SMP_EXT2_NUM 4

static volatile int smp_ext2_fail;
static volatile uint32_t smp_ext2_barrier;

struct smp_ext2_arg {
    int id;
    char path[64];
};

static void smp_ext2_barrier_wait(void) {
    __sync_fetch_and_add(&smp_ext2_barrier, 1);
    if (smp_ext2_barrier == SMP_EXT2_NUM) {
        smp_ext2_barrier = 0;
        syscall(SYS_futex, (uint32_t *)&smp_ext2_barrier, FUTEX_WAKE, SMP_EXT2_NUM, NULL, NULL, 0);
    } else {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        syscall(SYS_futex, (uint32_t *)&smp_ext2_barrier, FUTEX_WAIT, 0, &ts, NULL, 0);
    }
}

static void *smp_ext2_worker(void *arg) {
    struct smp_ext2_arg *a = arg;
    char buf[64];

    /* Each thread writes a unique pattern to its file */
    int fd = open(a->path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) { printf("  [t%d] open fail: %d\n", a->id, errno); smp_ext2_fail = 1; return NULL; }

    char msg[32];
    int len = snprintf(msg, sizeof(msg), "Thread %d says SMP ext2!", a->id);
    ssize_t n = write(fd, msg, len);
    if (n != len) { printf("  [t%d] write fail: %zd/%d\n", a->id, n, len); smp_ext2_fail = 1; close(fd); return NULL; }
    close(fd);

    /* Barrier: wait for all threads to finish writing */
    smp_ext2_barrier_wait();

    /* Now re-open and verify */
    fd = open(a->path, O_RDONLY);
    if (fd < 0) { printf("  [t%d] re-open fail: %d\n", a->id, errno); smp_ext2_fail = 1; return NULL; }

    __builtin_memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { printf("  [t%d] read fail: %zd\n", a->id, n); smp_ext2_fail = 1; close(fd); return NULL; }
    close(fd);

    if (strcmp(buf, msg) != 0) {
        printf("  [t%d] mismatch: got '%s', expected '%s'\n", a->id, buf, msg);
        smp_ext2_fail = 1;
    }
    return NULL;
}

static void test_ext2_smp_mt(void) {
    TEST("SMP ext2 concurrent write/read (4 threads, separate files)");

    smp_ext2_fail = 0;
    smp_ext2_barrier = 0;
    pthread_t threads[SMP_EXT2_NUM];
    struct smp_ext2_arg args[SMP_EXT2_NUM];
    for (int i = 0; i < SMP_EXT2_NUM; i++) {
        args[i].id = i;
        snprintf(args[i].path, sizeof(args[i].path), "/mnt/smp_ext2_%d.dat", i);
        int err = pthread_create(&threads[i], NULL, smp_ext2_worker, &args[i]);
        if (err != 0) { printf("  [t%d] pthread_create fail: %d\n", i, err); smp_ext2_fail = 1; }
    }
    for (int i = 0; i < SMP_EXT2_NUM; i++)
        pthread_join(threads[i], NULL);

    if (smp_ext2_fail) { FAIL("SMP ext2"); goto cleanup; }
    PASS();

cleanup:
    for (int i = 0; i < SMP_EXT2_NUM; i++)
        unlink(args[i].path);
}

#define SMP_SHARED_NUM 4
#define SMP_SHARED_CHUNK 4096

static volatile int smp_shared_fail;
static volatile uint32_t smp_shared_barrier;

static volatile int smp_append_fail;
static volatile uint32_t smp_append_barrier;

struct smp_shared_arg {
    int id;
    int fd;
};

static void smp_shared_barrier_wait(void) {
    __sync_fetch_and_add(&smp_shared_barrier, 1);
    if (smp_shared_barrier == SMP_SHARED_NUM) {
        smp_shared_barrier = 0;
        syscall(SYS_futex, (uint32_t *)&smp_shared_barrier, FUTEX_WAKE, SMP_SHARED_NUM, NULL, NULL, 0);
    } else {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        syscall(SYS_futex, (uint32_t *)&smp_shared_barrier, FUTEX_WAIT, 0, &ts, NULL, 0);
    }
}

static void smp_append_barrier_wait(void) {
    __sync_fetch_and_add(&smp_append_barrier, 1);
    if (smp_append_barrier == SMP_SHARED_NUM) {
        smp_append_barrier = 0;
        syscall(SYS_futex, (uint32_t *)&smp_append_barrier, FUTEX_WAKE, SMP_SHARED_NUM, NULL, NULL, 0);
    } else {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        syscall(SYS_futex, (uint32_t *)&smp_append_barrier, FUTEX_WAIT, 0, &ts, NULL, 0);
    }
}

static void *smp_shared_worker(void *arg) {
    struct smp_shared_arg *a = arg;
    char buf[SMP_SHARED_CHUNK];

    __builtin_memset(buf, a->id + 'A', sizeof(buf));

    smp_shared_barrier_wait();

    off_t off = (off_t)a->id * SMP_SHARED_CHUNK;
    ssize_t n = pwrite(a->fd, buf, sizeof(buf), off);
    if (n != sizeof(buf)) {
        printf("  [t%d] pwrite(%lld) fail: %zd err=%d\n", a->id, (long long)off, n, errno);
        smp_shared_fail = 1;
    }
    return NULL;
}

static void test_ext2_smp_shared(void) {
    TEST("SMP ext2 concurrent write to same file, different offsets (4 threads)");

    smp_shared_fail = 0;
    smp_shared_barrier = 0;

    int fd = open("/mnt/smp_shared.dat", O_CREAT | O_RDWR, 0644);
    if (fd < 0) { FAIL("open /mnt/smp_shared.dat"); return; }

    pthread_t threads[SMP_SHARED_NUM];
    struct smp_shared_arg args[SMP_SHARED_NUM];
    for (int i = 0; i < SMP_SHARED_NUM; i++) {
        args[i].id = i;
        args[i].fd = fd;
        int err = pthread_create(&threads[i], NULL, smp_shared_worker, &args[i]);
        if (err != 0) { printf("  [t%d] pthread_create fail: %d\n", i, err); smp_shared_fail = 1; }
    }
    for (int i = 0; i < SMP_SHARED_NUM; i++)
        pthread_join(threads[i], NULL);

    if (smp_shared_fail) { close(fd); unlink("/mnt/smp_shared.dat"); FAIL("SMP shared"); return; }

    char verify[SMP_SHARED_CHUNK];
    for (int i = 0; i < SMP_SHARED_NUM; i++) {
        off_t off = (off_t)i * SMP_SHARED_CHUNK;
        __builtin_memset(verify, 0, sizeof(verify));
        ssize_t n = pread(fd, verify, sizeof(verify), off);
        if (n != SMP_SHARED_CHUNK) {
            printf("  verify t%d: read %zd at off %lld\n", i, n, (long long)off);
            smp_shared_fail = 1; break;
        }
        for (int j = 0; j < SMP_SHARED_CHUNK; j++) {
            if (verify[j] != (char)(i + 'A')) {
                printf("  t%d byte %d: got 0x%02x, expected 0x%02x\n", i, j, verify[j], (unsigned char)(i + 'A'));
                smp_shared_fail = 1; break;
            }
        }
        if (smp_shared_fail) break;
    }

    close(fd);
    unlink("/mnt/smp_shared.dat");
    if (smp_shared_fail) { FAIL("SMP shared"); return; }
    PASS();
}

struct smp_append_arg {
    int id;
    int fd;
};

static void *smp_append_worker(void *arg) {
    struct smp_append_arg *a = arg;
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "  [t%d] append record\n", a->id);

    smp_append_barrier_wait();

    ssize_t n = write(a->fd, buf, len);
    if (n != len) {
        printf("  [t%d] append write fail: %zd/%d\n", a->id, n, len);
        smp_append_fail = 1;
    }
    return NULL;
}

static void test_ext2_smp_append(void) {
    TEST("SMP ext2 concurrent APPEND to same file (4 threads)");

    smp_append_fail = 0;
    smp_append_barrier = 0;

    int fd = open("/mnt/smp_append.dat", O_CREAT | O_RDWR, 0644);
    if (fd < 0) { FAIL("open /mnt/smp_append.dat"); return; }

    pthread_t threads[SMP_SHARED_NUM];
    struct smp_append_arg args[SMP_SHARED_NUM];
    for (int i = 0; i < SMP_SHARED_NUM; i++) {
        args[i].id = i;
        args[i].fd = fd;
        int err = pthread_create(&threads[i], NULL, smp_append_worker, &args[i]);
        if (err != 0) { printf("  [t%d] pthread_create fail: %d\n", i, err); smp_append_fail = 1; }
    }
    for (int i = 0; i < SMP_SHARED_NUM; i++)
        pthread_join(threads[i], NULL);

    if (smp_append_fail) { close(fd); unlink("/mnt/smp_append.dat"); FAIL("SMP append"); return; }

    char verify[1024];
    __builtin_memset(verify, 0, sizeof(verify));
    ssize_t total = pread(fd, verify, sizeof(verify) - 1, 0);
    if (total <= 0) { close(fd); unlink("/mnt/smp_append.dat"); FAIL("SMP append verify read"); return; }
    verify[total] = '\0';

    int records = 0;
    for (char *p = verify; *p; p++) {
        if (strncmp(p, "  [t", 4) == 0) records++;
    }
    if (records != SMP_SHARED_NUM) {
        printf("  expected %d records, got %d\n", SMP_SHARED_NUM, records);
        printf("  content:\n%s\n", verify);
        close(fd); unlink("/mnt/smp_append.dat"); FAIL("SMP append record count"); return;
    }

    close(fd);
    unlink("/mnt/smp_append.dat");
    PASS();
}

int main(void) {
    open("/dev/tty", O_RDWR);
    open("/dev/tty", O_RDWR);
    open("/dev/tty", O_RDWR);

    printf("=== Comprehensive stress test ===\n\n");

    check_stdout("start");

    test_kmem_stress();
    check_stdout("after kmem_stress");
    test_kmem_realloc();
    check_stdout("after kmem_realloc");
    test_pipe_large_rw();
    check_stdout("after pipe_large_rw");
    test_pipe_select();
    check_stdout("after pipe_select");
    // test_scm_multi_fd();
    check_stdout("after scm_multi_fd");
    test_epoll_et();
    check_stdout("after epoll_et");
    test_epoll_oneshot();
    check_stdout("after epoll_oneshot");
    test_futex_wake();
    check_stdout("after futex_wake");
    test_futex_timeout();
    check_stdout("after futex_timeout");

    test_fs_file_ops();
    check_stdout("after fs_file_ops");
    test_fs_stat();
    check_stdout("after fs_stat");
    test_fs_mkdir_getdents();
    check_stdout("after fs_mkdir_getdents");
    test_signal_handler();
    check_stdout("after signal_handler");
    test_signal_sigaction();
    check_stdout("after signal_sigaction");
    test_mmap_prot();
    check_stdout("after mmap_prot");
    test_mremap();
    check_stdout("after mremap");
    test_poll_basic();
    check_stdout("after poll_basic");
    test_eventfd();
    check_stdout("after eventfd");
    test_clock_gettime();
    check_stdout("after clock_gettime");
    test_multithreaded();
    check_stdout("after multithreaded");
    test_mt_concurrent_vm();
    check_stdout("after mt_concurrent_vm");
    test_mt_concurrent_epoll();
    check_stdout("after mt_concurrent_epoll");
    test_disk_rw();
    test_ext2_read();
    test_ext2_write();
    test_ext2_smp_mt();
    test_ext2_smp_shared();
    test_ext2_smp_append();

    int sockets[NUM_CHILDREN][2];
    pid_t pids[NUM_CHILDREN];
    int read_socks[NUM_CHILDREN];
    int cross_pipe[NUM_CHILDREN][2];

    printf("Children=%d  Iterations=%d  SHM=%luMB  Timeout=%ds\n",
           NUM_CHILDREN, ITERATIONS,
           SHM_SIZE / (1024UL * 1024UL), TIMEOUT_SEC);

    int start_fds = count_my_fds();

    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets[i]) < 0) {
            perror("socketpair"); return 1;
        }
        read_socks[i] = sockets[i][0];
        if (pipe(cross_pipe[i]) < 0) { perror("pipe"); return 1; }
    }

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pids[i] = fork();
        if (pids[i] < 0) { perror("fork"); return 1; }
        if (pids[i] == 0) {
            close(sockets[i][0]);
            close(cross_pipe[i][0]);
            child_client(sockets[i][1], i, cross_pipe[i][1]);
            _exit(0);
        } else {
            close(sockets[i][1]);
            close(cross_pipe[i][1]);
        }
    }

    parent_compositor(read_socks);

    printf("[Parent] Waiting for all children to report done...\n");
    uint8_t done_mask = 0;
    while (done_mask != (1u << NUM_CHILDREN) - 1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = 0;
        for (int i = 0; i < NUM_CHILDREN; i++) {
            if (!(done_mask & (1u << i))) {
                FD_SET(cross_pipe[i][0], &rfds);
                if (cross_pipe[i][0] > maxfd) maxfd = cross_pipe[i][0];
            }
        }
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            printf("[FAIL] Cross-check select error!\n");
            break;
        }
        if (r == 0) {
            printf("[FAIL] Cross-check timeout! mask=0x%02x\n", done_mask);
            break;
        }
        for (int i = 0; i < NUM_CHILDREN; i++) {
            if (!(done_mask & (1u << i)) && FD_ISSET(cross_pipe[i][0], &rfds)) {
                uint8_t val;
                ssize_t n = read(cross_pipe[i][0], &val, 1);
                if (n == 1) {
                    if (val == (uint8_t)i) {
                        done_mask |= (1u << i);
                    } else {
                        printf("[FAIL] Cross-check wrong token from child %d "
                               "(got %d)\n", i, val);
                    }
                } else if (n == 0) {
                    done_mask |= (1u << i);
                }
                close(cross_pipe[i][0]);
            }
        }
    }

    int end_fds = count_my_fds();

    printf("[Parent] FD count: %d -> %d (delta %d)\n",
           start_fds, end_fds, end_fds - start_fds);

    printf("[INIT] Copying initrd to ext2...\n");

    mkdir("/mnt/bin", 0755);
    mkdir("/mnt/lib", 0755);
    mkdir("/mnt/etc", 0755);
    mkdir("/mnt/dev", 0755);
    mkdir("/mnt/dev/dri", 0755);
    mkdir("/mnt/tmp", 1777);
    mkdir("/mnt/proc", 0555);
    mkdir("/mnt/usr", 0755);

    mknod("/mnt/dev/null", S_IFCHR | 0666, (1 << 8) | 3);
    mknod("/mnt/dev/tty",  S_IFCHR | 0666, (5 << 8) | 0);
    mknod("/mnt/dev/fb0",  S_IFCHR | 0666, (29 << 8) | 0);
    mknod("/mnt/dev/kbd",  S_IFCHR | 0666, (13 << 8) | 64);
    mknod("/mnt/dev/dri/card0", S_IFCHR | 0666, (226 << 8) | 0);

    static const char *copy_pairs[][2] = {
        { "/bin/init",                 "/mnt/bin/init" },
        { "/lib/libc.so",              "/mnt/lib/libc.so" },
        { "/lib/ld-musl-x86_64.so.1",  "/mnt/lib/ld-musl-x86_64.so.1" },
        { "/etc/hostname",             "/mnt/etc/hostname" },
    };
    for (int i = 0; i < 4; i++) {
        const char *src = copy_pairs[i][0];
        const char *dst = copy_pairs[i][1];
        int sfd = open(src, O_RDONLY);
        if (sfd < 0) {
            printf("  skip %s (open err %d)\n", src, errno);
            continue;
        }
        int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (dfd < 0) {
            printf("  skip %s -> %s (create err %d)\n", src, dst, errno);
            close(sfd);
            continue;
        }
        char buf[4096];
        ssize_t n;
        while ((n = read(sfd, buf, sizeof(buf))) > 0) {
            ssize_t written = write(dfd, buf, n);
            if (written != n) {
                printf("  write error copying %s\n", src);
                break;
            }
        }
        close(sfd);
        close(dfd);
        printf("  copied %s -> %s\n", src, dst);
    }

    printf("[INIT] Calling pivot_root(/mnt, /mnt/oldroot)...\n");
    int pivot_ret = syscall(SYS_pivot_root, "/mnt", "/mnt/oldroot");
    if (pivot_ret == 0) {
        printf("[INIT] pivot_root OK: ext2 is now root, old root at /oldroot\n");
    } else {
        printf("[INIT] pivot_root failed: %d\n", pivot_ret);
        return 0;
    }

    struct stat root_st, old_st, init_st;
    if (stat("/", &root_st) == 0 && stat("/oldroot", &old_st) == 0) {
        printf("[ROOT] /  (new root)      dev=%ju ino=%ju\n",
               (uintmax_t)root_st.st_dev, (uintmax_t)root_st.st_ino);
        printf("[ROOT] /oldroot (old root) dev=%ju ino=%ju\n",
               (uintmax_t)old_st.st_dev, (uintmax_t)old_st.st_ino);
        if (root_st.st_dev != old_st.st_dev)
            printf("[ROOT] Different mount points: pivot_root confirmed!\n");
        else
            printf("[ROOT] WARNING: same device — pivot_root may not have worked\n");
    }

    int fd = open("/pivot_test_marker.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "ext2 root works!\n", 17);
        close(fd);
        printf("[ROOT] Created /pivot_test_marker.txt on ext2\n");
    }

    if (stat("/bin/init", &init_st) == 0) {
        printf("[ROOT] /bin/init from ext2: size=%ju, ino=%ju\n",
               (uintmax_t)init_st.st_size, (uintmax_t)init_st.st_ino);
    }
    struct stat old_init_st;
    if (stat("/oldroot/bin/init", &old_init_st) == 0) {
        printf("[ROOT] /oldroot/bin/init from ramfs: size=%ju, ino=%ju\n",
               (uintmax_t)old_init_st.st_size, (uintmax_t)old_init_st.st_ino);
    }

    printf("[ROOT] Syncing filesystems...\n");
    syscall(SYS_sync);
    printf("[ROOT] pivot_root demo complete. Halting.\n");
    return 0;
}
