#include "filesys/free-map.h"

#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

#include <bitmap.h>
#include <debug.h>

static struct file *free_map_file; /* Free map file. */
static struct bitmap *free_map;    /* Free map, one bit per sector. */
static block_sector_t free_map_sector = UINT32_MAX;
static block_sector_t root_directory_sector = UINT32_MAX;

/* Initializes the free map. */
void
free_map_init(block_sector_t inofile_sector_count)
{
	const block_size_t total_blocks = block_size(fs_device);
	free_map = bitmap_create(total_blocks);
	if (free_map == NULL)
		PANIC("bitmap creation failed--file system device is too "
		      "large");

	free_map_sector = INOFILE_SECTOR + inofile_sector_count;
	const block_sector_t free_map_sector_count =
		DIV_ROUND_UP(bitmap_file_size(free_map), BLOCK_SECTOR_SIZE);
	bitmap_set_multiple(free_map,
	                    free_map_sector,
	                    free_map_sector_count,
	                    true);

	root_directory_sector = free_map_sector + free_map_sector_count;
	bitmap_mark(free_map, root_directory_sector);
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
	if (sector != BITMAP_ERROR && free_map_file != NULL &&
	    !bitmap_write(free_map, free_map_file)) {
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
	bitmap_write(free_map, free_map_file);
}

/* Opens the free map file and reads it from disk. */
void
free_map_open(void)
{
	free_map_file = file_open(inode_open(FREE_MAP_SECTOR));
	if (free_map_file == NULL)
		PANIC("can't open free map");
	if (!bitmap_read(free_map, free_map_file))
		PANIC("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
void
free_map_close(void)
{
	file_close(free_map_file);
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void
free_map_create(void)
{
	/* Create inode. */
	if (!inode_create(FREE_MAP_SECTOR, bitmap_file_size(free_map)))
		PANIC("free map creation failed");

	/* Write bitmap to file. */
	free_map_file = file_open(inode_open(FREE_MAP_SECTOR));
	if (free_map_file == NULL)
		PANIC("can't open free map");
	if (!bitmap_write(free_map, free_map_file))
		PANIC("can't write free map");
}
