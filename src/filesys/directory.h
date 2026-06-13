#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include "devices/block.h"
#include "filesys/inode.h"

#include <stdbool.h>
#include <stddef.h>

/* Maximum length of a file name component.
   This is the traditional UNIX maximum length.
   After directories are implemented, this maximum length may be
   retained, but much longer full path names must be allowed. */
#define NAME_MAX 14

struct inode;

/* Opening and closing directories. */
bool dir_create_root(void);
struct dir *dir_open(struct inode *); // TODO rm, hide struct inode
struct dir *dir_open_root(void);
struct dir *dir_reopen(struct dir *);
void dir_close(struct dir *);
struct inode *dir_get_inode(struct dir *); // TODO rm, hide struct inode

/* Reading and writing. */
// TODO: add struct file **, struct dir ** params, hide underlying inode
bool dir_lookup(char *path, struct inode **inode);
bool dir_add(char *path, off_t length);
bool dir_mkdir(char *path);
bool dir_remove(char *path);
bool dir_readdir(struct dir *, char name[NAME_MAX + 1]);

#endif /* filesys/directory.h */
