#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include "devices/block.h"
#include "filesys/off_t.h"

#include <stdbool.h>

/* Sectors of system file inodes. */
#define INOFILE_SECTOR 0

/* Block device that contains the file system. */
extern struct block *fs_device;

void filesys_init(bool format);
void filesys_done(void);
bool filesys_create(char *path, off_t initial_size);
struct file *filesys_open(char *path);
bool filesys_remove(char *path);

#endif /* filesys/filesys.h */
