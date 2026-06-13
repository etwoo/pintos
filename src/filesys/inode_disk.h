#ifndef FILESYS_INODE_DISK_H
#define FILESYS_INODE_DISK_H

#include "filesys/inode.h"

/* Maximum number of supported inodes (files). */
extern const block_sector_t INODE_LIMIT;

/* Error value returned by byte_to_sector() and similar APIs. */
extern const block_sector_t INODE_SECTOR_UNSET;

/* Multi-level index of blocks belonging to a given inode. */
struct inode_disk_index {
	block_sector_t direct[12];  /* Direct blocks containing file data. */
	block_sector_t indirect;    /* Location of indirect block. */
	block_sector_t indirect_2x; /* Location of double indirect block. */
};

/* On-disk inode. Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
	struct inode_disk_index idx;
	off_t length;         /* File size in bytes. */
	uint32_t flags;       /* INODE_FLAG_* values. */
	uint32_t refcnt;      /* TODO: Number of hardlinks to this inode. */
	uint32_t magic;       /* Magic number. */
	uint32_t unused[110]; /* Not used. */
};

/* Returns the block device sector that contains byte offset POS within INODE.
   Returns INODE_SECTOR_UNSET if INODE does not contain data at offset POS. */
block_sector_t byte_to_sector(const struct inode *inode, off_t pos, bool alloc);

bool inode_disk_create(off_t length, uint32_t flags, ino_t *out);
bool inode_disk_check(ino_t ino, uint32_t flags);
bool inode_disk_isdir(ino_t ino);
off_t inode_disk_to_length(ino_t ino);
bool inode_disk_set_length(ino_t ino, off_t length);

#endif
