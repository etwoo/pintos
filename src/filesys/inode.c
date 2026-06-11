#include "filesys/inode.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

#include <array.h>
#include <debug.h>
#include <list.h>
#include <round.h>
#include <string.h>

const ino_t ROOT_DIRECTORY_INO = 0;

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

struct inode_disk_index {
	block_sector_t direct[12];  /* Direct blocks containing file data. */
	block_sector_t indirect;    /* Location of indirect block. */
	block_sector_t indirect_2x; /* Location of double indirect block. */
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
	struct inode_disk_index idx;
	off_t length;         // TODO: change to uint32_t
	uint32_t refcnt;      /* Number of hardlinks to this inode. */
	uint32_t magic;       /* Magic number. */
	uint32_t unused[111]; /* Not used. */
};

struct inode_disk_indirect {
	block_sector_t blocks[128];
};

struct inode_disk_index *TYPE_INDEX = NULL; /* Type system shenanigans. */
struct inode_disk_indirect *TYPE_INDIRECT = NULL;
static const off_t SPAN_INDIRECT =
	BLOCK_SECTOR_SIZE * ARRAY_SIZE(TYPE_INDIRECT->blocks);
static const off_t MAX_DIRECT =
	BLOCK_SECTOR_SIZE * ARRAY_SIZE(TYPE_INDEX->direct);
static const off_t MAX_INDIRECT = MAX_DIRECT + SPAN_INDIRECT;
static const off_t MAX_INDIRECT_2x =
	MAX_INDIRECT + SPAN_INDIRECT * ARRAY_SIZE(TYPE_INDIRECT->blocks);

static const block_sector_t BLOCK_SECTOR_INVALID = UINT32_MAX;

static block_sector_t
ino_to_inode_disk_sector(ino_t ino)
{
	ASSERT(sizeof(struct inode_disk) == BLOCK_SECTOR_SIZE);
	return INOFILE_SECTOR + ino;
}

static block_sector_t
ino_to_inode_disk_member(ino_t ino, size_t n)
{
	const block_sector_t sector = ino_to_inode_disk_sector(ino);

	block_sector_t out = 0;
	ASSERT(n < sizeof(struct inode_disk_index) / sizeof(out));
	const int pos = n * sizeof(out);

	if (!cache_read(sector, pos, sizeof(out), &out)) {
		return BLOCK_SECTOR_INVALID;
	}
	return out;
}

/* In-memory inode. */
struct inode {
	struct list_elem elem; /* Element in inode list. */
	ino_t ino;
	int open_cnt;       /* Number of openers. */
	bool removed;       /* True if deleted, false otherwise. */
	int deny_write_cnt; /* 0: writes ok, >0: deny writes. */
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns BLOCK_SECTOR_INVALID if INODE does not contain data for a byte at
   offset POS. */
static block_sector_t
byte_to_sector(const struct inode *inode, off_t pos)
{
	ASSERT(inode != NULL);

	if (pos < MAX_DIRECT) {
		const size_t n = pos / BLOCK_SECTOR_SIZE;
		ASSERT(n < ARRAY_SIZE(TYPE_INDEX->direct));
		return ino_to_inode_disk_member(inode->ino, n);
	} else if (pos < MAX_INDIRECT) {
		const block_sector_t indirect =
			ino_to_inode_disk_member(inode->ino, 12);
		if (indirect == BLOCK_SECTOR_INVALID) {
			return BLOCK_SECTOR_INVALID;
		}
		const off_t relpos = pos - MAX_DIRECT;
		const size_t slot = relpos / BLOCK_SECTOR_SIZE;
		ASSERT(slot < ARRAY_SIZE(TYPE_INDIRECT->blocks));
		block_sector_t out = 0;
		const size_t sz = sizeof(out);
		if (!cache_read(indirect, slot * sz, sz, &out)) {
			return BLOCK_SECTOR_INVALID;
		}
		return out;
	}

	// TODO: consolidate byte_to_sector() copy-pasta above and below
	ASSERT(pos < MAX_INDIRECT_2x);
	const block_sector_t indirect_2x =
		ino_to_inode_disk_member(inode->ino, 13);
	if (indirect_2x == BLOCK_SECTOR_INVALID) {
		return BLOCK_SECTOR_INVALID;
	}
	const off_t relpos_indirect = pos - MAX_INDIRECT;
	const size_t slot_indirect = relpos_indirect / SPAN_INDIRECT;
	ASSERT(slot_indirect < ARRAY_SIZE(TYPE_INDIRECT->blocks));
	block_sector_t indirect_1x = 0;
	const size_t sz = sizeof(indirect_1x);
	if (!cache_read(indirect_2x, slot_indirect * sz, sz, &indirect_1x)) {
		return BLOCK_SECTOR_INVALID;
	}
	const off_t relpos_direct = (relpos_indirect % SPAN_INDIRECT);
	const size_t slot_direct = relpos_direct / BLOCK_SECTOR_SIZE;
	ASSERT(slot_direct < ARRAY_SIZE(TYPE_INDIRECT->blocks));
	block_sector_t out = 0;
	ASSERT(sizeof(out) == sz);
	if (!cache_read(indirect_1x, slot_direct * sz, sz, &out)) {
		return BLOCK_SECTOR_INVALID;
	}
	return out;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. Returns next free sector after inofile. */
block_sector_t
inode_init(void)
{
	list_init(&open_inodes);

	/* Allocate 4% of sectors to the inofile. With an 8MB disk,
	 * the inofile supports 8*1024*1024/512/25 = 655 files. */
	return INOFILE_SECTOR + block_size(fs_device) / 25;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create(off_t length, ino_t *ino)
{
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	static ino_t inode_allocator = ROOT_DIRECTORY_INO;
	*ino = inode_allocator++; // TODO: pick free ino in inofile

	ASSERT(length >= 0);

	/* If this assertion fails, the inode structure is not exactly
	   one sector in size, and you should fix that. */
	ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

	disk_inode = calloc(1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		// TODO: mark slot/sector as used in inofile?
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		const int sz = BLOCK_SECTOR_SIZE;
		const block_sector_t sector = ino_to_inode_disk_sector(*ino);
		success = cache_write(sector, 0, sz, disk_inode);
		free(disk_inode);
		disk_inode = NULL;
	}
	return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open(ino_t ino)
{
	struct list_elem *e;
	struct inode *inode;

	// TODO: do we need a lock around open_inodes as part of sync revamp?
	/* Check whether this inode is already open. */
	for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
	     e = list_next(e)) {
		inode = list_entry(e, struct inode, elem);
		if (inode->ino == ino) {
			inode_reopen(inode);
			return inode;
		}
	}

	/* Allocate memory. */
	inode = malloc(sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front(&open_inodes, &inode->elem);
	inode->ino = ino;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen(struct inode *inode)
{
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* Returns INODE's inode number. */
ino_t
inode_get_inumber(const struct inode *inode)
{
	return inode->ino;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close(struct inode *inode)
{
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove(&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			// TODO: free_map_release(), if free_map_allocate() was
			// called lazily; check on-disk structures? free both
			// slot in on-disk inofile and associated direct,
			// indirect, indirect_2x blocks?
		}

		free(inode);
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove(struct inode *inode)
{
	ASSERT(inode != NULL);
	inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at(struct inode *inode, void *buffer, off_t size, off_t offset)
{
	off_t bytes_read = 0;

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		block_sector_t sector_idx = byte_to_sector(inode, offset);
		if (sector_idx == INOFILE_SECTOR || // TODO cleanup
		    sector_idx == BLOCK_SECTOR_INVALID) {
			break;
		}
		int sector_ofs = offset % BLOCK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two.
		 */
		off_t inode_left = inode_length(inode) - offset;
		int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
		int min_left =
			inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		/* Read sector (or chunk within sector) via buffer cache. */
		if (!cache_read(sector_idx,
		                sector_ofs,
		                chunk_size,
		                buffer + bytes_read)) {
			break;
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at(struct inode *inode,
               const void *buffer,
               off_t size,
               off_t offset)
{
	off_t bytes_written = 0;

	if (inode->deny_write_cnt)
		return 0;

	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		block_sector_t sector_idx = byte_to_sector(inode, offset);
		if (sector_idx == INOFILE_SECTOR || // TODO cleanup
		    sector_idx == BLOCK_SECTOR_INVALID) {
			// TODO: allocate block lazily, update ino in inofile
			// lack of above is why filesys_create() -> crashloop!
			break;
		}

		int sector_ofs = offset % BLOCK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two.
		 */
		off_t inode_left = inode_length(inode) - offset;
		int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
		int min_left =
			inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		/* Write sector (or chunk within sector) via buffer cache. */
		if (!cache_write(sector_idx,
		                 sector_ofs,
		                 chunk_size,
		                 buffer + bytes_written)) {
			break;
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}

	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write(struct inode *inode)
{
	inode->deny_write_cnt++;
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write(struct inode *inode)
{
	ASSERT(inode->deny_write_cnt > 0);
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length(const struct inode *inode)
{
	const block_sector_t sector = ino_to_inode_disk_sector(inode->ino);
	const int pos = sizeof(*TYPE_INDEX); // TODO: reconsider hacks

	off_t out = 0;
	if (!cache_read(sector, pos, sizeof(out), &out)) {
		return -1;
	}
	return out;
}
