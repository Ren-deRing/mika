#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <uapi/types.h>
#include <uapi/sys/ipc.h>

struct trapframe;
struct proc;
struct shmid_ds;
struct sembuf;

bool is_user_address_range(const void *addr, size_t size);
int copy_to_user(void *user_dest, const void *src, size_t n);
int copy_from_user(void *dest, const void *user_src, size_t n);

// sys_file.c
int64_t sys_read(int fd, void *user_buf, size_t count);
int64_t sys_write(int fd, const void *user_buf, size_t count);
int64_t sys_open(const char *user_path, int flags, int mode);
int64_t sys_close(int fd);
int64_t sys_fstat(int fd, void *user_stat);
int64_t sys_lseek(int fd, int64_t offset, int whence);
int64_t sys_ioctl(int fd, uint64_t request, void *arg);
int64_t sys_readv(int fd, const void *iov, int iovcnt);
int64_t sys_writev(int fd, const void *iov, int iovcnt);
int64_t sys_fcntl(int fd, int cmd, uint64_t arg);
int64_t sys_newfstatat(int dfd, const char *path, void *statbuf, int flag);

// sys_proc.c
int64_t sys_getpid(void);
int64_t sys_fork(void);
int64_t sys_clone(uint64_t flags, void *child_stack, void *ptid, void *ctid, uint64_t newtls);
int64_t sys_execve(const char *pathname, char *const argv[], char *const envp[]);
int64_t sys_exit(int status);
int64_t sys_wait4(pid_t pid, int *wstatus, int options, void *rusage);
int64_t sys_set_tid_address(int *tidptr);

// sys_mem.c
int64_t sys_brk(uintptr_t addr);
int64_t sys_mmap(uintptr_t addr, size_t length, int prot, int flags, int fd, int64_t offset);
int64_t sys_munmap(void *addr, size_t length);
int64_t sys_mprotect(uintptr_t start, size_t len, int prot);

// sys_sig.c
int64_t sys_rt_sigaction(int sig, const void *act, void *oact, size_t sigsetsize);
int64_t sys_rt_sigprocmask(int how, const void *set, void *oset, size_t sigsetsize);
int64_t sys_rt_sigreturn(void);
int64_t sys_kill(pid_t pid, int sig);
void    handle_signal(struct trapframe *tf);

// sys_sync.c
int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, const void *timeout, uint32_t *uaddr2, uint32_t val3);
int64_t sys_clock_gettime(int clock_id, void *tp);
int64_t sys_getrlimit(int resource, void *user_rlim);
int64_t sys_arch_prctl(int code, uint64_t addr);

// sys_shm.c, sys_sem.c
int64_t sys_shmget(key_t key, size_t size, int shmflg);
void*   sys_shmat(int shmid, const void *shmaddr, int shmflg);
int64_t sys_shmdt(const void *shmaddr);
int64_t sys_shmctl(int shmid, int cmd, struct shmid_ds *buf);
int64_t sys_semget(key_t key, int nsems, int semflg);
int64_t sys_semop(int semid, struct sembuf *sops, size_t nsops);
int64_t sys_semctl(int semid, int semnum, int cmd, uint64_t arg);
