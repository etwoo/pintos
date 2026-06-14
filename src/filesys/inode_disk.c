#include "filesys/inode_disk.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

#include <array.h>
#include <debug.h>

const block_sector_t INODE_LIMIT = 512; /* Note: dir-vine test needs >=406 */
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
cache_read_or_alloc(block_sector_t sector, int pos, struct lock *alloc)
{
	bool success = false;
	void *zeroes = NULL;

	block_sector_t out = INODE_SECTOR_UNSET;
	if (!cache_read(sector, pos, sizeof(out), &out)) {
		ASSERT(out == INODE_SECTOR_UNSET);
	}

	if (out != INODE_SECTOR_UNSET) {
		success = true;
		goto done;
	}

	if (alloc == NULL || !free_map_allocate(1, &out)) {
		goto done;
	}

	ASSERT(BLOCK_SECTOR_SIZE <= PGSIZE);
	zeroes = palloc_get_page(PAL_ZERO);
	if (zeroes == NULL) {
		goto done;
	}

	/* Initialize allocated sector to all zeroes. */
	if (!cache_write(out, 0, BLOCK_SECTOR_SIZE, zeroes)) {
		goto done;
	}

	/* Use test-and-test-and-set when updating the inofile to refer to
	 * newly-allocated sectors. Use caller-provided, per-inode lock to
	 * prevent concurrent writers from clobbering one another's writes and
	 * orphaning sectors, i.e. creating marked-as-used sectors that are not
	 * actually reachable from the inofile. */
	lock_acquire(alloc);

	block_sector_t test = INODE_SECTOR_UNSET;
	if (cache_read(sector, pos, sizeof(test), &test) &&
	    test == INODE_SECTOR_UNSET) {
		/* Update inofile to refer to allocated sector. */
		success = cache_write(sector, pos, sizeof(out), &out);
	} else if (test != INODE_SECTOR_UNSET) {
		/* Return sector allocated by another concurrent writer. */
		out = test;
		success = true;
	}

	lock_release(alloc);

done:
	if (!success && out != INODE_SECTOR_UNSET) {
		/* Undo partial change on failure. */
		free_map_release(out, 1);
		/* Return error to caller. */
		out = INODE_SECTOR_UNSET;
	}
	palloc_free_page(zeroes);
	return out;
}

static block_sector_t
get_inode_disk_member(ino_t ino, size_t slot, struct lock *alloc)
{
	const block_sector_t sector = ino_to_inode_disk_sector(ino);

	ASSERT(slot < sizeof(struct inode_disk_index) / sizeof(block_sector_t));
	const int pos = slot * sizeof(block_sector_t);

	return cache_read_or_alloc(sector, pos, alloc);
}

static block_sector_t
get_indirect_block_slot(block_sector_t indirect,
                        off_t filepos,
                        struct lock *alloc)
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
                           struct lock *alloc)
{
	ASSERT(MAX_INDIRECT <= filepos && filepos < MAX_INDIRECT_2x);
	const off_t relpos = filepos - MAX_INDIRECT;

	const size_t slot = relpos / SPAN_INDIRECT;
	ASSERT(slot < ARRAY_SIZE(TYPE_INDIRECT->blocks));

	const int pos = slot * sizeof(TYPE_INDIRECT->blocks[0]);
	return cache_read_or_alloc(indirect_2x, pos, alloc);
}

static block_sector_t
byte_to_sector_direct(ino_t ino, off_t pos, struct lock *alloc)
{
	ASSERT(pos < MAX_DIRECT);

	const size_t slot = pos / BLOCK_SECTOR_SIZE;
	ASSERT(slot < ARRAY_SIZE(TYPE_INDEX->direct));
	return get_inode_disk_member(ino, slot, alloc);
}

static block_sector_t
byte_to_sector_indirect(ino_t ino, off_t pos, struct lock *alloc)
{
	ASSERT(MAX_DIRECT <= pos && pos < MAX_INDIRECT);

	const block_sector_t indirect = get_inode_disk_member(ino, 12, alloc);
	if (indirect == INODE_SECTOR_UNSET) {
		return INODE_SECTOR_UNSET;
	}

	return get_indirect_block_slot(indirect, pos, alloc);
}

static block_sector_t
byte_to_sector_indirect_2x(ino_t ino, off_t pos, struct lock *alloc)
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
byte_to_sector(ino_t ino, off_t pos, struct lock *alloc)
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
	*out = INODE_SECTOR_UNSET;
	bool success = false;

	struct inode_disk *to_write = palloc_get_page(PAL_ZERO);
	if (to_write == NULL) {
		goto done;
	}
	ASSERT(sizeof(*to_write) <= PGSIZE);
	ASSERT(sizeof(*to_write) == BLOCK_SECTOR_SIZE);

	to_write->length = length;
	to_write->flags = flags;
	to_write->magic = INODE_MAGIC;

	if (!inode_map_allocate(1, out)) {
		goto done;
	}
	ASSERT(*out < INODE_LIMIT);

	const block_sector_t sector = ino_to_inode_disk_sector(*out);
	success = cache_write(sector, 0, BLOCK_SECTOR_SIZE, to_write);

done:
	if (!success && *out != INODE_SECTOR_UNSET) {
		/* Undo partial change on failure. */
		inode_map_release(*out, 1);
		/* Return error to caller. */
		*out = INODE_SECTOR_UNSET;
	}
	palloc_free_page(to_write);
	return success;
}

void
inode_disk_unlink(ino_t ino)
{
	const off_t length = inode_disk_to_length(ino);

	for (off_t pos = 0; pos < length; pos += BLOCK_SECTOR_SIZE) {
		block_sector_t sector = byte_to_sector(ino, pos, NULL);
		if (sector != INODE_SECTOR_UNSET) {
			free_map_release(sector, 1);
		} /* else: no work to do for hole in sparse file. */
	}

	inode_map_release(ino_to_inode_disk_sector(ino), 1);
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

	if ((flags & INODE_FLAG_IS_DIR) != 0 && !inode_disk_isdir(ino)) {
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

	return ((flags & INODE_FLAG_IS_DIR) != 0);
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
