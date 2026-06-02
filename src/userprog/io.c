#include "userprog/io.h"

#include "threads/synch.h"

static struct lock io_lock;

void
io_init(void)
{
	lock_init(&io_lock);
}

void
acquire_io_lock(void)
{
	lock_acquire(&io_lock);
}

void
release_io_lock(void)
{
	lock_release(&io_lock);
}

bool
io_lock_held_by_current_thread(void)
{
	return lock_held_by_current_thread(&io_lock);
}
