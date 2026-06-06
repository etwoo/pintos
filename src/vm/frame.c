#include "vm/frame.h"

#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"

#include <hash.h>
#include <list.h>

// TODO: As a concrete example, you must not allow page faults to occur while a
// device driver accesses a user buffer passed to file_read, because you would
// not be able to invoke the driver while handling such faults. Preventing such
// page faults requires cooperation between the code within which the access
// occurs and your page eviction code. For instance, you could extend your frame
// table to record when a page contained in a frame must not be evicted. (This
// is also referred to as "pinning" or "locking" the page in its frame.) Pinning
// restricts your page replacement algorithm's choices when looking for pages to
// evict, so be sure to pin pages no longer than necessary, and avoid pinning
// pages when it is not necessary.

struct frame {
	tid_t owner;
	void *kpage;
	void *upage;
	struct list_elem elem;
};

struct frame_table {
	struct lock lock;
	struct list table;
};

static struct frame_table ft; /* frames that contain a user page. */

void
frame_init(void)
{
	lock_init(&ft.lock);
	list_init(&ft.table);
}

static void *
frame_get_page_maybe_swap(enum palloc_flags flags)
{
	void *kpage = palloc_get_page(flags | PAL_USER);
	if (kpage != NULL) {
		return kpage;
	}

	tid_t victim_tid = TID_ERROR;
	void *victim_upage = NULL;
	{
		lock_acquire(&ft.lock);
		ASSERT(!list_empty(&ft.table));
		// TODO: second-chance replacement instead of pure FIFO
		// TODO: avoid kpages in-use by syscall handler; pinning?
		struct list_elem *e = list_pop_front(&ft.table);
		struct frame *fr = list_entry(e, struct frame, elem);
		victim_tid = fr->owner;
		victim_upage = fr->upage;
		lock_release(&ft.lock);
		free(fr);
	}
	page_evict(victim_tid, victim_upage);

	// TODO: handle other thread racing, allocating between when we free up
	// a page and when we allocate ourselves
	//
	// reusing an existing kpage is complicated because process_exit()
	// deallocates via page_destructor() and pagedir_destroy(); seems
	// simpler to avoid lifetime issues
	kpage = palloc_get_page(flags | PAL_USER);
	ASSERT(kpage != NULL && "Out-of-memory even after swapping");
	return kpage;
}

void *
frame_get_page(void *upage, enum palloc_flags flags, enum page_rw rw)
{
	struct thread *t = thread_current();
	ASSERT(pagedir_get_page(t->pagedir, upage) == NULL);

	struct frame *fr = malloc(sizeof(*fr));
	if (fr == NULL) {
		return NULL;
	}

	void *kpage = frame_get_page_maybe_swap(flags);
	ASSERT(kpage != NULL);

	fr->owner = t->tid;
	fr->kpage = kpage;
	fr->upage = upage;

	const bool writable = (rw == PAGE_WRITABLE);
	if (!pagedir_set_page(t->pagedir, upage, kpage, writable)) {
		palloc_free_page(kpage);
		free(fr);
		return NULL;
	}

	lock_acquire(&ft.lock);
	list_push_back(&ft.table, &fr->elem);
	lock_release(&ft.lock);

	return kpage;
}
