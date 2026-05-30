#ifndef USERPROG_FILE_DESCRIPTOR_H
#define USERPROG_FILE_DESCRIPTOR_H

#include <list.h>

#define FD_INVALID -1

struct fdtable_entry {
	int fd;
	struct file *file;
	struct list_elem elem; /* List element for file descriptor table. */
};

int fd_register(struct file *file);
void fd_unregister(int fd);
struct file *fd_to_file(int fd);

#endif
