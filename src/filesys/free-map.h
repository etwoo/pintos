#ifndef FILESYS_FREE_MAP_H
#define FILESYS_FREE_MAP_H

#include "devices/block.h"

#include <stdbool.h>
#include <stddef.h>

void free_map_init(block_sector_t start_sector);
void free_map_create(void);
void free_map_open(void);
void free_map_close(void);

bool free_map_allocate(size_t, block_sector_t *);
void free_map_release(block_sector_t, size_t);

bool inode_map_allocate(size_t, block_sector_t *);
void inode_map_release(block_sector_t, size_t);

#endif /* filesys/free-map.h */
