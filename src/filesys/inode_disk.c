#include "filesys/inode_disk.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

#include <array.h>
#include <debug.h>

const block_sector_t INODE_LIMIT = 1900; // TODO: dir-vine needs high limit
const block_sector_t INODE_SECTOR_UNSET = 0;
static const uint32_t INODE_MAGIC = 0x494e4f44;

struct inode_disk_indirect {
	block_sector_t blocks[128];
};

struct inode_disk_index *TYPE_INDEX = NULL;       /* Type system shenanigans. */
struct inode_disk_indirect *TYPE_INDIRECT = NULL; /* Type system shenanigans. */
static const off_t SPAN_INDIRECT =
	BLOCK_SECTOR_SIZE * ARRAY_SIZE(TYPE_INDIRECT->blocks);
static const off_t MAX_DIRECT =
	BLOCK_SECTOR_SIZE * ARRAY_SIZE(TYPE_INDEX->direct);
static const off_t MAX_INDIRECT = MAX_DIRECT + SPAN_INDIRECT;
static const off_t MAX_INDIRECT_2x =
	MAX_INDIRECT + SPAN_INDIRECT * ARRAY_SIZE(TYPE_INDIRECT->blocks);

static block_sector_t
ino_to_inode_disk_sector(ino_t ino)
{
	ASSERT(sizeof(struct inode_disk) == BLOCK_SECTOR_SIZE);
	return INOFILE_SECTOR + ino;
}

static block_sector_t
cache_read_or_alloc(block_sector_t sector, int pos, bool alloc)
{
	block_sector_t out = INODE_SECTOR_UNSET;
	if (!cache_read(sector, pos, sizeof(out), &out)) {
		ASSERT(out == INODE_SECTOR_UNSET);
	}
	if (alloc && out == INODE_SECTOR_UNSET && free_map_allocate(1, &out)) {
		ASSERT(BLOCK_SECTOR_SIZE <= PGSIZE);
		void *zeroes = palloc_get_page(PAL_ZERO);
		if (zeroes == NULL ||
		    /* Update containing block to refer to allocated sector. */
		    !cache_write(sector, pos, sizeof(out), &out) ||
		    /* Initialize allocated sector to all zeroes. */
		    !cache_write(out, 0, BLOCK_SECTOR_SIZE, zeroes)) {
			free_map_release(out, 1);
			out = INODE_SECTOR_UNSET;
		}
		palloc_free_page(zeroes);
	}
	return out;
}

static block_sector_t
get_inode_disk_member(ino_t ino, size_t slot, bool alloc)
{
	const block_sector_t sector = ino_to_inode_disk_sector(ino);

	ASSERT(slot < sizeof(struct inode_disk_index) / sizeof(block_sector_t));
	const int pos = slot * sizeof(block_sector_t);

	return cache_read_or_alloc(sector, pos, alloc);
}

static block_sector_t
get_indirect_block_slot(block_sector_t indirect, off_t filepos, bool alloc)
{
	off_t relpos = 0;
	if (MAX_DIRECT <= filepos && filepos < MAX_INDIRECT) {
		relpos = filepos - MAX_DIRECT;
	} else if (MAX_INDIRECT <= filepos && filepos <= MAX_INDIRECT_2x) {
		relpos = filepos % SPAN_INDIRECT;
	} else {
		ASSERT(false && "Out-of-range arg to get_indirect_block_slot");
	}

	const size_t slot = relpos / BLOCK_SECTOR_SIZE;
	ASSERT(slot < ARRAY_SIZE(TYPE_INDIRECT->blocks));

	const int pos = slot * sizeof(TYPE_INDIRECT->blocks[0]);
	return cache_read_or_alloc(indirect, pos, alloc);
}

static block_sector_t
get_indirect_2x_block_slot(block_sector_t indirect_2x,
                           off_t filepos,
                           bool alloc)
{
	ASSERT(MAX_INDIRECT <= filepos && filepos < MAX_INDIRECT_2x);
	const off_t relpos = filepos - MAX_INDIRECT;

	const size_t slot = relpos / SPAN_INDIRECT;
	ASSERT(slot < ARRAY_SIZE(TYPE_INDIRECT->blocks));

	const int pos = slot * sizeof(TYPE_INDIRECT->blocks[0]);
	return cache_read_or_alloc(indirect_2x, pos, alloc);
}

static block_sector_t
byte_to_sector_direct(ino_t ino, off_t pos, bool alloc)
{
	ASSERT(pos < MAX_DIRECT);

	const size_t slot = pos / BLOCK_SECTOR_SIZE;
	ASSERT(slot < ARRAY_SIZE(TYPE_INDEX->direct));
	return get_inode_disk_member(ino, slot, alloc);
}

static block_sector_t
byte_to_sector_indirect(ino_t ino, off_t pos, bool alloc)
{
	ASSERT(MAX_DIRECT <= pos && pos < MAX_INDIRECT);

	const block_sector_t indirect =
		get_inode_disk_member(ino, 12, alloc);
	if (indirect == INODE_SECTOR_UNSET) {
		return INODE_SECTOR_UNSET;
	}

	return get_indirect_block_slot(indirect, pos, alloc);
}

static block_sector_t
byte_to_sector_indirect_2x(ino_t ino, off_t pos, bool alloc)
{
	ASSERT(MAX_INDIRECT <= pos && pos < MAX_INDIRECT_2x);

	const block_sector_t indirect_2x =
		get_inode_disk_member(ino, 13, alloc);
	if (indirect_2x == INODE_SECTOR_UNSET) {
		return INODE_SECTOR_UNSET;
	}

	const block_sector_t indirect_1x =
		get_indirect_2x_block_slot(indirect_2x, pos, alloc);
	if (indirect_1x == INODE_SECTOR_UNSET) {
		return INODE_SECTOR_UNSET;
	}

	return get_indirect_block_slot(indirect_1x, pos, alloc);
}

block_sector_t
byte_to_sector(ino_t ino, off_t pos, bool alloc)
{
	block_sector_t out = INODE_SECTOR_UNSET;
	if (pos < MAX_DIRECT) {
		out = byte_to_sector_direct(ino, pos, alloc);
	} else if (pos < MAX_INDIRECT) {
		out = byte_to_sector_indirect(ino, pos, alloc);
	} else {
		out = byte_to_sector_indirect_2x(ino, pos, alloc);
	}

	return out;
}

bool
inode_disk_create(off_t length, uint32_t flags, ino_t *out)
{
	ASSERT(length >= 0);

	struct inode_disk *i = palloc_get_page(PAL_ZERO);
	if (i == NULL) {
		return -1;
	}
	ASSERT(sizeof(*i) <= PGSIZE);
	ASSERT(sizeof(*i) == BLOCK_SECTOR_SIZE);

	i->length = length;
	i->flags = flags;
	i->magic = INODE_MAGIC;

	if (!inode_map_allocate(1, out)) {
		return false;
	}
	ASSERT(*out < INODE_LIMIT);

	const block_sector_t sector = ino_to_inode_disk_sector(*out);
	const bool success = cache_write(sector, 0, BLOCK_SECTOR_SIZE, i);
	palloc_free_page(i);
	return success;
}

bool
inode_disk_check(ino_t ino, uint32_t flags)
{
	const block_sector_t sector = ino_to_inode_disk_sector(ino);
	const int pos = offsetof(struct inode_disk, magic);

	uint32_t magic = 0;
	if (!cache_read(sector, pos, sizeof(magic), &magic) ||
	    magic != INODE_MAGIC) {
		return false;
	}

	if ((flags & INODE_FLAG_IS_DIRECTORY) != 0 && !inode_disk_isdir(ino)) {
		return false;
	}

	return true;
}

bool
inode_disk_isdir(ino_t ino)
{
	const block_sector_t sector = ino_to_inode_disk_sector(ino);
	const int pos = offsetof(struct inode_disk, flags);

	uint32_t flags = 0;
	if (!cache_read(sector, pos, sizeof(flags), &flags)) {
		return -1;
	}

	return ((flags & INODE_FLAG_IS_DIRECTORY) != 0);
}

off_t
inode_disk_to_length(ino_t ino)
{
	const block_sector_t sector = ino_to_inode_disk_sector(ino);
	const int pos = offsetof(struct inode_disk, length);

	off_t out = 0;
	if (!cache_read(sector, pos, sizeof(out), &out)) {
		return -1;
	}
	return out;
}

bool
inode_disk_set_length(ino_t ino, off_t length)
{
	const block_sector_t sector = ino_to_inode_disk_sector(ino);
	const int pos = offsetof(struct inode_disk, length);
	return cache_write(sector, pos, sizeof(length), &length);
}
