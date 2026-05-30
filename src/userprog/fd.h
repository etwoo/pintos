#ifndef USERPROG_FILE_DESCRIPTOR_H
#define USERPROG_FILE_DESCRIPTOR_H

#include <list.h>

#define FD_INVALID -1

struct fdtable_entry {
	int fd;
	struct file *file;
	struct list_elem elem; /* List element for file descriptor table. */
};

int fd_create(struct file *file);

#endif
