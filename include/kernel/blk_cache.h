#pragma once

#include <stdint.h>

int blk_cache_read(int dev_id, uint64_t sector, void *buf);
int blk_cache_write(int dev_id, uint64_t sector, const void *buf);
void blk_cache_flush(int dev_id);
void blk_cache_flush_all(void);
