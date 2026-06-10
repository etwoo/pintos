#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"

void cache_init(int64_t writeback_period_ms);
void cache_done(void);

bool cache_read(block_sector_t sector, int pos, int sz, void *buffer);
void cache_write(block_sector_t sector, const void *buffer);

#endif
