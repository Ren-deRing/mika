#pragma once

#include <stdint.h>

#define SYS_test 0
#define SYS_write 1
#define SYS_brk 2

int64_t syscall(uint64_t num, ...);
