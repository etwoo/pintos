#include "filesys/inode.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/inode_disk.h"
#include "threads/malloc.h"
#include "threads/synch.h"

#include <list.h>
#include <string.h>

const ino_t ROOT_DIRECTORY_INO = 0;
const uint32_t INODE_FLAG_IS_DIRECTORY = 0x1;

/* In-memory inode. */
struct inode {
	struct lock lock;
	ino_t ino;             /* Unique on-disk and in-memory identifier. */
	int open_cnt;          /* Number of openers. */
	bool removed;          /* True if deleted, false otherwise. */
	int deny_write_cnt;    /* 0: writes ok, >0: deny writes. */
	struct list_elem elem; /* Element in open_inodes list. */
};

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct lock open_inodes_lock;
static struct list open_inodes;

/* Initializes the inode module. Returns next free sector after inofile. */
block_sector_t
inode_init(void)
{
	lock_init(&open_inodes_lock);
	list_init(&open_inodes);
	return INOFILE_SECTOR + INODE_LIMIT;
}

bool
inode_check(ino_t ino, uint32_t flags)
{
	return inode_disk_check(ino, flags);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create(off_t length, uint32_t flags, ino_t *ino)
{
	return inode_disk_create(length, flags, ino);
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open(ino_t ino)
{
	lock_acquire(&open_inodes_lock);

	struct inode *inode = NULL;

	/* Check whether this inode is already open. */
	struct list_elem *e = list_begin(&open_inodes);
	for (; e != list_end(&open_inodes); e = list_next(e)) {
		inode = list_entry(e, struct inode, elem);
		if (inode->ino == ino) {
			inode_reopen(inode);
			goto done;
		}
	}

	/* Allocate memory. */
	inode = malloc(sizeof *inode);
	if (inode == NULL) {
		goto done;
	}

	/* Initialize. */
	lock_init(&inode->lock);
	inode->ino = ino;
	inode->open_cnt = 1;
	inode->removed = false;
	inode->deny_write_cnt = 0;
	list_push_front(&open_inodes, &inode->elem);

done:
	lock_release(&open_inodes_lock);
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen(struct inode *inode)
{
	if (inode != NULL) {
		lock_acquire(&inode->lock);
		inode->open_cnt++;
		lock_release(&inode->lock);
	}
	return inode;
}

/* Returns INODE's inode number. */
ino_t
inode_get_inumber(struct inode *inode)
{
	lock_acquire(&inode->lock);
	const ino_t ino = inode->ino;
	lock_release(&inode->lock);

	return ino;
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

	struct inode *safe_to_free = NULL;

	lock_acquire(&open_inodes_lock);
	lock_acquire(&inode->lock);

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove(&inode->elem);
		safe_to_free = inode;
	}

	lock_release(&inode->lock);
	lock_release(&open_inodes_lock);

	if (safe_to_free != NULL) {
		/* Deallocate blocks if removed. */
		if (safe_to_free->removed) {
			// TODO: inode_map_release() for inofile slot
			// TODO: free_map_release() for direct/indirect blocks
		}
		free(safe_to_free);
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove(struct inode *inode)
{
	ASSERT(inode != NULL);
	lock_acquire(&inode->lock);
	inode->removed = true;
	lock_release(&inode->lock);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at(struct inode *inode, void *buffer, off_t size, off_t offset)
{
	lock_acquire(&inode->lock);
	const ino_t ino = inode->ino;
	lock_release(&inode->lock);
	inode = NULL; /* Do not access struct inode after this point. */

	const off_t length = inode_disk_to_length(ino);
	if (length < 0) {
		return -1;
	}

	off_t bytes_read = 0;

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		block_sector_t sector_idx = byte_to_sector(ino, offset, false);
		const bool sparse = (sector_idx == INODE_SECTOR_UNSET);
		int sector_ofs = offset % BLOCK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two.
		 */
		off_t inode_left = length - offset;
		int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
		int min_left =
			inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		/* Read sector (or chunk within sector) via buffer cache. */
		if (sparse) {
			/* Return all-zero values from hole in sparse file. */
			memset(buffer + bytes_read, 0, chunk_size);
		} else if (!cache_read(sector_idx,
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
	lock_acquire(&inode->lock);
	const ino_t ino = inode->ino;
	const bool deny_write = (inode->deny_write_cnt > 0);
	lock_release(&inode->lock);
	inode = NULL; /* Do not access struct inode after this point. */

	const off_t offset_begin = offset;
	off_t bytes_written = 0;

	if (deny_write) {
		return 0;
	}

	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		block_sector_t sector_idx = byte_to_sector(ino, offset, true);
		if (sector_idx == INODE_SECTOR_UNSET) {
			break;
		}

		int sector_ofs = offset % BLOCK_SECTOR_SIZE;
		int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < sector_left ? size : sector_left;
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

	if (offset_begin + bytes_written > inode_disk_to_length(ino)) {
		inode_disk_set_length(ino, offset_begin + bytes_written);
	}
	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write(struct inode *inode)
{
	lock_acquire(&inode->lock);
	inode->deny_write_cnt++;
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);
	lock_release(&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write(struct inode *inode)
{
	lock_acquire(&inode->lock);
	ASSERT(inode->deny_write_cnt > 0);
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
	lock_release(&inode->lock);
}

bool
inode_is_removed(struct inode *inode)
{
	lock_acquire(&inode->lock);
	const bool removed = inode->removed;
	lock_release(&inode->lock);

	return removed;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length(struct inode *inode)
{
	return inode_disk_to_length(inode_get_inumber(inode));
}

bool
inode_isdir(struct inode *inode)
{
	return inode_disk_isdir(inode_get_inumber(inode));
}
