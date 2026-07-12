#pragma once

#include <stdint.h>

#define MSI_ADDR_BASE 0xFEE00000ULL

struct msix_table_entry {
    uint64_t msg_addr;
    uint32_t msg_data;
    uint32_t vector_ctrl;
} __attribute__((packed));

struct msix_capability {
    uint8_t  cap_id;
    uint8_t  next_cap;
    uint16_t msg_ctrl;
    uint32_t table_offset;  // bir[0:2] | offset[3:31]
    uint32_t pba_offset;    // bir[0:2] | offset[3:31]
} __attribute__((packed));

#define MSIX_CTRL_TABLE_SIZE(c)  (((c) & 0x07FF) + 1)
#define MSIX_CTRL_FUNCTION_MASK  (1 << 14)
#define MSIX_CTRL_ENABLE         (1 << 15)

#define MSIX_TABLE_BIR(o)   ((o) & 0x07)
#define MSIX_TABLE_OFFSET(o) ((o) & ~0x07)

#define MSIX_VECTORCTRL_MASK (1 << 0)
