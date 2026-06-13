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
#ifndef FILESYS
	lock_acquire(&io_lock);
#endif
}

void
release_io_lock(void)
{
#ifndef FILESYS
	lock_release(&io_lock);
#endif
}

void
assert_io_lock_held_by_current_thread(void)
{
#ifndef FILESYS
	ASSERT(lock_held_by_current_thread(&io_lock));
#endif
}
