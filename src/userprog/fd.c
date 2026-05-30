#include "userprog/fd.h"

#include "threads/malloc.h"
#include "threads/thread.h"

int
fd_create(struct file *file)
{
	struct thread *t = thread_current();
	struct fdtable_entry *fde = malloc(sizeof(*fde));
	fde->fd = t->fd_generator++;
	fde->file = file;
	list_push_back(&t->fd_table, &fde->elem);
	return fde->fd;
}
