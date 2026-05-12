#include <stdint.h>
#include <sys/syscall.h>

void _start() {
    // syscall 0: test
    int64_t result = syscall(0, 0xDEADBEEF, 0, 0);

    // syscall 1: write(fd=1, buf, len)
    const char *msg = "Hello from userspace!\n";
    uint64_t len = 0;
    while (msg[len]) len++;

    syscall(1, 1, (uint64_t)msg, len);

    while (1);
}