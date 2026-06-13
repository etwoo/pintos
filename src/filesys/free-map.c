#include "filesys/free-map.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/inode_disk.h"
#include "threads/malloc.h"
#include "threads/synch.h"

#include <bitmap.h>
#include <debug.h>
#include <round.h>

struct serializable_bitmap {
	struct lock lock;
	void *memory;
	struct bitmap *bitmap;
	block_sector_t sector;
	block_sector_t sector_count;
};

static struct serializable_bitmap inode_map; /* Free map, one bit per inode. */
static struct serializable_bitmap block_map; /* Free map, one bit per sector. */

static void
map_init(struct serializable_bitmap *sb, size_t bitcount)
{
	ASSERT(sb->sector != 0);
	lock_init(&sb->lock);

	const size_t buffer_size = bitmap_buf_size(bitcount);
	sb->sector_count = DIV_ROUND_UP(buffer_size, BLOCK_SECTOR_SIZE);

	sb->memory = malloc(buffer_size);
	if (sb->memory == NULL) {
		PANIC("bitmap creation failed -- too many inodes or blocks");
	}

	sb->bitmap = bitmap_create_in_buf(bitcount, sb->memory, buffer_size);
	ASSERT(sb->bitmap != NULL);
}

/* Initializes the free map. */
void
free_map_init(block_sector_t start_sector)
{
	inode_map.sector = start_sector;
	map_init(&inode_map, INODE_LIMIT);

	block_map.sector = inode_map.sector + inode_map.sector_count;
	map_init(&block_map, block_size(fs_device));

	bitmap_set_multiple(block_map.bitmap,
	                    INOFILE_SECTOR,
	                    block_map.sector + block_map.sector_count,
	                    true);
}

static bool
map_write(struct serializable_bitmap *sb)
{
	ASSERT(sb->memory != NULL);
	ASSERT(sb->sector != 0);
	ASSERT(sb->sector_count != 0);
	bool ok = true;

	/* Blit structure to disk. Would be better to deal with endianness and
	 * generally make serialization more portable. Live with it for now. */
	for (block_sector_t i = 0; i < sb->sector_count; ++i) {
		/* It is probably safe to access the memory of underlying
		   bitmap elements without taking a lock because bitmap
		   elements are set atomically (though testing them is not
		   atomic with setting them). In other words, we may write the
		   bitmap in an intermediate state, but it won't be in a
		   totally unreadable/unrecognizable state. */
		const void *buf = sb->memory + (i * BLOCK_SECTOR_SIZE);
		if (!cache_write(sb->sector + i, 0, BLOCK_SECTOR_SIZE, buf)) {
			ok = false;
		}
	}

	return ok;
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
static bool
map_allocate(struct serializable_bitmap *sb, size_t cnt, block_sector_t *out)
{
	lock_acquire(&sb->lock);
	block_sector_t sector = bitmap_scan_and_flip(sb->bitmap, 0, cnt, false);
	lock_release(&sb->lock);

	if (sector != BITMAP_ERROR && !map_write(sb)) {
		lock_acquire(&sb->lock);
		ASSERT(bitmap_all(sb->bitmap, sector, cnt));
		bitmap_set_multiple(sb->bitmap, sector, cnt, false);
		lock_release(&sb->lock);
		sector = BITMAP_ERROR;
	}
	if (sector != BITMAP_ERROR)
		*out = sector;
	return sector != BITMAP_ERROR;
}

bool
inode_map_allocate(size_t cnt, block_sector_t *sectorp)
{
	return map_allocate(&inode_map, cnt, sectorp);
}

bool
free_map_allocate(size_t cnt, block_sector_t *sectorp)
{
	return map_allocate(&block_map, cnt, sectorp);
}

/* Makes CNT sectors starting at SECTOR available for use. */
static void
map_release(struct serializable_bitmap *sb, block_sector_t sector, size_t cnt)
{
	lock_acquire(&sb->lock);
	ASSERT(bitmap_all(sb->bitmap, sector, cnt));
	bitmap_set_multiple(sb->bitmap, sector, cnt, false);
	lock_release(&sb->lock);

	map_write(sb);
}

void
inode_map_release(block_sector_t sector, size_t cnt)
{
	map_release(&inode_map, sector, cnt);
}

void
free_map_release(block_sector_t sector, size_t cnt)
{
	map_release(&block_map, sector, cnt);
}

static void
map_open(struct serializable_bitmap *sb)
{
	ASSERT(sb->memory != NULL);
	ASSERT(sb->sector != 0);
	ASSERT(sb->sector_count != 0);

	/* See free_map_write(). */
	for (block_sector_t i = 0; i < sb->sector_count; ++i) {
		void *buf = sb->memory + (i * BLOCK_SECTOR_SIZE);
		(void)cache_read(sb->sector + i, 0, BLOCK_SECTOR_SIZE, buf);
	}
}

/* Opens the map files and reads them from disk. */
void
free_map_open(void)
{
	map_open(&inode_map);
	map_open(&block_map);
}

/* Writes the map files to disk. */
void
free_map_close(void)
{
	map_write(&inode_map);
	map_write(&block_map);
}

/* Creates new map files on disk and writes them. */
void
free_map_create(void)
{
	map_write(&inode_map);
	map_write(&block_map);
}
