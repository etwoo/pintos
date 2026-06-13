#include "filesys/filesys.h"

#include "filesys/cache.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"

#include <debug.h>
#include <stdio.h>
#include <string.h>

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format(void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init(bool format)
{
	fs_device = block_get_role(BLOCK_FILESYS);
	if (fs_device == NULL)
		PANIC("No file system device found, can't initialize file "
		      "system.");

	cache_init();
	free_map_init(inode_init());

	if (format) {
		do_format();
	} else if (!inode_check(ROOT_DIRECTORY_INO)) {
		PANIC("File system lacks valid root directory.");
	}

	free_map_open();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done(void)
{
	free_map_close();
	cache_done();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create(char *path, off_t sz)
{
	return dir_add(path, sz);
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open(char *path)
{
	struct inode *inode = NULL;
	if (!dir_lookup(path, &inode)) {
		return NULL;
	}
	return file_open(inode); /* Takes ownership. */
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove(char *path)
{
	return dir_remove(path);
}

/* Formats the file system. */
static void
do_format(void)
{
	printf("Formatting file system...");
	free_map_create();
	if (!dir_create())
		PANIC("root directory creation failed");
	free_map_close();
	printf("done.\n");
}
