#include "filesys/free-map.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/inode_disk.h"

#include <bitmap.h>
#include <debug.h>
#include <round.h>

static struct bitmap *inode_map; /* Free map, one bit per inode. */
static block_sector_t inode_map_sector = UINT32_MAX;
static block_sector_t inode_map_sector_count = UINT32_MAX;

static struct bitmap *free_map; /* Free map, one bit per sector. */
static block_sector_t free_map_sector = UINT32_MAX;
static block_sector_t free_map_sector_count = UINT32_MAX;

/* Initializes the free map. */
void
free_map_init(block_sector_t start_sector)
{
	inode_map_sector = start_sector;

	inode_map = bitmap_create(INODE_LIMIT);
	if (inode_map == NULL) {
		PANIC("bitmap creation failed -- too many inodes");
	}
	inode_map_sector_count =
		DIV_ROUND_UP(bitmap_file_size(inode_map), BLOCK_SECTOR_SIZE);

	free_map_sector = inode_map_sector + inode_map_sector_count;
	;

	free_map = bitmap_create(block_size(fs_device));
	if (free_map == NULL) {
		PANIC("bitmap creation failed -- block device is too large");
	}

	free_map_sector_count =
		DIV_ROUND_UP(bitmap_file_size(free_map), BLOCK_SECTOR_SIZE);
	bitmap_set_multiple(free_map,
	                    INOFILE_SECTOR,
	                    free_map_sector + free_map_sector_count,
	                    true);
}

static const uint32_t WRITE_INODE_MAP = 0x1;
static const uint32_t WRITE_FREE_MAP = 0x2;

static bool
free_map_write(uint32_t flags)
{
	ASSERT(inode_map_sector != UINT32_MAX);
	ASSERT(inode_map_sector_count != UINT32_MAX);
	ASSERT(free_map_sector != UINT32_MAX);
	ASSERT(free_map_sector_count != UINT32_MAX);
	const bool imap = ((flags & WRITE_INODE_MAP) != 0);
	const bool fmap = ((flags & WRITE_FREE_MAP) != 0);
	const int sz = BLOCK_SECTOR_SIZE;
	bool ok = true;

	/* Blit structure to disk. Would be better to deal with endianness and
	 * generally make serialization more portable. Live with it for now. */

	for (block_sector_t i = 0; imap && i < inode_map_sector_count; ++i) {
		const void *buf = ((void *)inode_map) + (i * BLOCK_SECTOR_SIZE);
		ok = cache_write(inode_map_sector + i, 0, sz, buf) && ok;
	}

	for (block_sector_t i = 0; fmap && i < free_map_sector_count; ++i) {
		const void *buf = ((void *)free_map) + (i * BLOCK_SECTOR_SIZE);
		ok = cache_write(free_map_sector + i, 0, sz, buf) && ok;
	}

	return ok;
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
static bool
map_allocate(uint32_t flags, size_t cnt, block_sector_t *sectorp)
{
	const bool imap = ((flags & WRITE_INODE_MAP) != 0);
	struct bitmap *bm = imap ? inode_map : free_map;

	block_sector_t sector = bitmap_scan_and_flip(bm, 0, cnt, false);
	if (sector != BITMAP_ERROR && !free_map_write(flags)) {
		bitmap_set_multiple(bm, sector, cnt, false);
		sector = BITMAP_ERROR;
	}
	if (sector != BITMAP_ERROR)
		*sectorp = sector;
	return sector != BITMAP_ERROR;
}

bool
inode_map_allocate(size_t cnt, block_sector_t *sectorp)
{
	return map_allocate(WRITE_INODE_MAP, cnt, sectorp);
}

bool
free_map_allocate(size_t cnt, block_sector_t *sectorp)
{
	return map_allocate(WRITE_FREE_MAP, cnt, sectorp);
}

/* Makes CNT sectors starting at SECTOR available for use. */
static void
map_release(uint32_t flags, block_sector_t sector, size_t cnt)
{
	const bool imap = ((flags & WRITE_INODE_MAP) != 0);
	struct bitmap *bm = imap ? inode_map : free_map;

	ASSERT(bitmap_all(bm, sector, cnt));
	bitmap_set_multiple(bm, sector, cnt, false);

	free_map_write(flags);
}

void inode_map_release(block_sector_t sector, size_t cnt)
{
	map_release(WRITE_INODE_MAP, sector, cnt);
}

void free_map_release(block_sector_t sector, size_t cnt)
{
	map_release(WRITE_FREE_MAP, sector, cnt);
}

/* Opens the map files and reads them from disk. */
void
free_map_open(void)
{
	ASSERT(inode_map_sector != UINT32_MAX);
	ASSERT(inode_map_sector_count != UINT32_MAX);
	ASSERT(free_map_sector != UINT32_MAX);
	ASSERT(free_map_sector_count != UINT32_MAX);
	const int sz = BLOCK_SECTOR_SIZE;

	/* See free_map_write(). */

	for (block_sector_t i = 0; i < inode_map_sector_count; ++i) {
		void *buf = ((void *)inode_map) + (i * BLOCK_SECTOR_SIZE);
		(void)cache_read(inode_map_sector + i, 0, sz, buf);
	}

	for (block_sector_t i = 0; i < free_map_sector_count; ++i) {
		void *buf = ((void *)free_map) + (i * BLOCK_SECTOR_SIZE);
		(void)cache_read(free_map_sector + i, 0, sz, buf);
	}
}

/* Writes the map files to disk. */
void
free_map_close(void)
{
	free_map_write(WRITE_INODE_MAP | WRITE_FREE_MAP);
}

/* Creates new map files on disk and writes them. */
void
free_map_create(void)
{
	free_map_write(WRITE_INODE_MAP | WRITE_FREE_MAP);
}
