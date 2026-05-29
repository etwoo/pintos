#include "userprog/fd.h"

#include "threads/malloc.h"
#include "threads/thread.h"

int
fd_register(struct file *file)
{
	struct thread *t = thread_current();
	struct fdtable_entry *fde = malloc(sizeof(*fde));
	fde->fd = t->fd_generator++;
	fde->file = file;
	list_push_back(&t->fd_table, &fde->elem);
	return fde->fd;
}

void
fd_unregister(int fd)
{
	struct thread *t = thread_current();

	struct list_elem *e = list_begin(&t->fd_table);
	for (; e != list_end(&t->fd_table); e = list_next(e)) {
		struct fdtable_entry *fde =
			list_entry(e, struct fdtable_entry, elem);
		if (fd == fde->fd) {
			list_remove(&fde->elem);
			free(fde);
			break;
		}
	}
}

struct file *
fd_to_file(int fd)
{
	struct thread *t = thread_current();

	struct list_elem *e = list_begin(&t->fd_table);
	for (; e != list_end(&t->fd_table); e = list_next(e)) {
		struct fdtable_entry *fde =
			list_entry(e, struct fdtable_entry, elem);
		if (fd == fde->fd) {
			return fde->file;
		}
	}

	return NULL;
}
