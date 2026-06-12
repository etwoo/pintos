#include "filesys/inode.h"

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/inode_disk.h"
#include "filesys/inode_inmem.h"
#include "threads/malloc.h"

#include <string.h>

const ino_t ROOT_DIRECTORY_INO = 0;
const uint32_t INODE_FLAG_IS_DIRECTORY = 0x1;

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
		block_sector_t sector_idx =
			byte_to_sector(inode, offset, false);
		const bool sparse = (sector_idx == INODE_SECTOR_UNSET);
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
	const off_t offset_begin = offset;
	off_t bytes_written = 0;

	if (inode->deny_write_cnt)
		return 0;

	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		block_sector_t sector_idx = byte_to_sector(inode, offset, true);
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

	if (offset_begin + bytes_written > inode_length(inode)) {
		inode_disk_set_length(inode->ino, offset_begin + bytes_written);
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
	return inode_disk_to_length(inode->ino);
}

bool
inode_isdir(const struct inode *inode)
{
	return inode_disk_isdir(inode->ino);
}
