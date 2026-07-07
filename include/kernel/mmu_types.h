#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ARCH_KERNEL_BASE 0xFFFF800000000000ULL

typedef uint64_t pgprot_t;
typedef uint64_t phys_addr_t;
typedef uint64_t vm_flags_t;

#define VM_READ  (1ULL << 0)
#define VM_WRITE (1ULL << 1)
#define VM_EXEC  (1ULL << 2)
#define VM_USER  (1ULL << 3)
