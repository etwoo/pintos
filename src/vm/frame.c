#include "vm/frame.h"

#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

#include <hash.h>
#include <list.h>

struct frame {
	tid_t owner;
	void *upage;
	bool pinned;
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

		// TODO: second-chance replacement instead of pure FIFO
		struct list_elem *e = list_begin(&ft.table);
		for (; e != list_end(&ft.table); e = list_next(e)) {
			struct frame *candidate =
				list_entry(e, struct frame, elem);
			if (!candidate->pinned) {
				list_remove(e); /* Pop first unpinned entry. */
				break;
			}
		}
		ASSERT(e != list_end(&ft.table));

		struct frame *fr = list_entry(e, struct frame, elem);
		victim_tid = fr->owner;
		victim_upage = fr->upage;

		ASSERT(!fr->pinned);
		free(fr);

		lock_release(&ft.lock);
	}
	thread_page_evict(victim_tid, victim_upage);

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
	fr->upage = upage;
	fr->pinned = false;

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

static void
frame_pin_set(tid_t tid, void *uaddr, size_t sz, bool to_pin)
{
	lock_acquire(&ft.lock);

	void *begin = pg_round_down(uaddr);
	void *end = pg_round_up(uaddr + sz) - 1;

	for (void *cursor = begin; cursor < end; cursor += PGSIZE) {
		struct list_elem *e = list_begin(&ft.table);
		for (; e != list_end(&ft.table); e = list_next(e)) {
			struct frame *maybe = list_entry(e, struct frame, elem);
			if (maybe->owner == tid && maybe->upage == cursor) {
				maybe->pinned = to_pin;
				break;
			}
		}
		if (to_pin && e == list_end(&ft.table)) {
			ASSERT(0 && "No frame to pin; already swapped out?");
		}
	}

	lock_release(&ft.lock);
}

void
frame_pin(tid_t tid, void *uaddr, size_t sz)
{
	frame_pin_set(tid, uaddr, sz, /* to_pin */ true);
}

void
frame_unpin(tid_t tid, void *uaddr, size_t sz)
{
	frame_pin_set(tid, uaddr, sz, /* to_pin */ false);
}

void
frame_clear(tid_t tid)
{
	struct list to_save;
	list_init(&to_save);

	lock_acquire(&ft.lock);

	while (!list_empty(&ft.table)) {
		struct list_elem *e = list_pop_front(&ft.table);
		struct frame *fr = list_entry(e, struct frame, elem);
		if (fr->owner == tid) {
			free(fr);
		} else {
			list_push_back(&to_save, e);
		}
	}

	list_splice(list_begin(&ft.table),
	            list_begin(&to_save),
	            list_end(&to_save));
	lock_release(&ft.lock);
}
