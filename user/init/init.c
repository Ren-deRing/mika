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

#define NUM_CHILDREN     4
#define ITERATIONS       500
#define SHM_SIZE  (4UL * 1024 * 1024)
#define TIMEOUT_SEC      50

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

int main() {
    open("/dev/tty", O_RDWR);
    open("/dev/tty", O_RDWR);
    open("/dev/tty", O_RDWR);

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
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
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
    return 0;
}
