#include "filesys/free-map.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

#include <bitmap.h>
#include <debug.h>
#include <round.h>

static struct bitmap *free_map; /* Free map, one bit per sector. */
static block_sector_t free_map_sector = UINT32_MAX;
static block_sector_t free_map_sector_count = UINT32_MAX;

/* Initializes the free map. */
void
free_map_init(block_sector_t free_map_sector_)
{
	free_map_sector = free_map_sector_;

	free_map = bitmap_create(block_size(fs_device));
	if (free_map == NULL)
		PANIC("bitmap creation failed--file system device is too "
		      "large");

	free_map_sector_count =
		DIV_ROUND_UP(bitmap_file_size(free_map), BLOCK_SECTOR_SIZE);
	bitmap_set_multiple(free_map,
	                    free_map_sector,
	                    free_map_sector_count,
	                    true);
}

static bool
free_map_write(void)
{
	ASSERT(free_map_sector != UINT32_MAX);
	ASSERT(free_map_sector_count != UINT32_MAX);
	const int sz = BLOCK_SECTOR_SIZE;
	bool result = true;

	/* Blit structure to disk. Would be better to deal with endianness and
	 * generally make serialization more portable. Live with it for now. */
	for (block_sector_t i = 0; i < free_map_sector_count; ++i) {
		const void *buf = ((void *)free_map) + (i * BLOCK_SECTOR_SIZE);
		result = cache_write(free_map_sector + i, 0, sz, buf) && result;
	}

	return result;
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool
free_map_allocate(size_t cnt, block_sector_t *sectorp)
{
	block_sector_t sector = bitmap_scan_and_flip(free_map, 0, cnt, false);
	if (sector != BITMAP_ERROR && !free_map_write()) {
		bitmap_set_multiple(free_map, sector, cnt, false);
		sector = BITMAP_ERROR;
	}
	if (sector != BITMAP_ERROR)
		*sectorp = sector;
	return sector != BITMAP_ERROR;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void
free_map_release(block_sector_t sector, size_t cnt)
{
	ASSERT(bitmap_all(free_map, sector, cnt));
	bitmap_set_multiple(free_map, sector, cnt, false);
	(void)free_map_write();
}

/* Opens the free map file and reads it from disk. */
void
free_map_open(void)
{
	ASSERT(free_map_sector != UINT32_MAX);
	ASSERT(free_map_sector_count != UINT32_MAX);
	const int sz = BLOCK_SECTOR_SIZE;

	/* See free_map_write(). */
	for (block_sector_t i = 0; i < free_map_sector_count; ++i) {
		void *buf = ((void *)free_map) + (i * BLOCK_SECTOR_SIZE);
		(void)cache_read(free_map_sector + i, 0, sz, buf);
	}
}

/* Writes the free map to disk and closes the free map file. */
void
free_map_close(void)
{
	free_map_write();
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void
free_map_create(void)
{
	free_map_write();
}
