#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include "devices/block.h"
#include "filesys/off_t.h"

#include <stdbool.h>

typedef uint32_t ino_t;

extern const ino_t ROOT_DIRECTORY_INO;
extern const uint32_t INODE_FLAG_IS_DIR;

block_sector_t inode_init(void);
bool inode_check(ino_t, uint32_t);
bool inode_create(off_t, uint32_t, ino_t *);
struct inode *inode_open(ino_t);
struct inode *inode_reopen(struct inode *);
ino_t inode_get_inumber(struct inode *);
void inode_close(struct inode *);
void inode_remove(struct inode *);
off_t inode_read_at(struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at(struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write(struct inode *);
void inode_allow_write(struct inode *);
off_t inode_length(struct inode *);
bool inode_isdir(struct inode *);

/* Less ergonomic APIs for use by directory.c. */
void inode_lock_acquire(struct inode *);
void inode_lock_release(struct inode *);
void inode_lock_held_by_current_thread(struct inode *);
bool inode_locked_is_removed(struct inode *);
off_t inode_locked_read_at(struct inode *, void *, off_t, off_t);
off_t inode_locked_write_at(struct inode *, const void *, off_t, off_t);

#endif /* filesys/inode.h */
