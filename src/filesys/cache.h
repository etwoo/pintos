#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"

void cache_init(void);
void cache_done(void);

bool cache_read(block_sector_t sector, int pos, int sz, void *buffer);
bool cache_write(block_sector_t sector, int pos, int sz, const void *buffer);

#endif
