#ifndef FILESYS_INODE_INTERNAL_H
#define FILESYS_INODE_INTERNAL_H

#include "filesys/inode.h"

#include <list.h>

/* In-memory inode. */
struct inode {
	struct list_elem elem; /* Element in inode list. */
	ino_t ino;
	int open_cnt;       /* Number of openers. */
	bool removed;       /* True if deleted, false otherwise. */
	int deny_write_cnt; /* 0: writes ok, >0: deny writes. */
};

#endif
