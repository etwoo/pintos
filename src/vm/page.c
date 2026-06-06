#include "vm/page.h"

#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/fd.h"
#include "userprog/io.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"

#include <string.h>

enum page_type {
	PAGE_ANONYMOUS,
	PAGE_FILE_BACKED,
};

static const int MMAP_ID_UNSET = -2;

struct page_entry {
	void *upage;
	enum palloc_flags flags;
	enum page_rw rw;
	enum page_type type;
	union {
		struct {
			struct swap_slot swap;
		} anon;
		struct {
			int mmap;
			int fd;
			off_t pos;
		} file;
	};
	struct hash_elem elem;        /* Hash element for page_table. */
	struct list_elem munmap_elem; /* See page_munmap(). */
};

static unsigned
page_hash(const struct hash_elem *e_, void *aux UNUSED)
{
	const struct page_entry *e = hash_entry(e_, struct page_entry, elem);
	return pg_no(e->upage);
}

static bool
page_less(const struct hash_elem *a_,
          const struct hash_elem *b_,
          void *aux UNUSED)
{
	const struct page_entry *a = hash_entry(a_, struct page_entry, elem);
	const struct page_entry *b = hash_entry(b_, struct page_entry, elem);
	return pg_no(a->upage) < pg_no(b->upage);
}

static void
page_evict_prepare(struct hash_elem *e_, bool swap_anonymous_memory)
{
	struct page_entry *entry = hash_entry(e_, struct page_entry, elem);

	struct thread *t = thread_current();
	ASSERT(lock_held_by_current_thread(&t->vm.lock));

	void *kpage = pagedir_get_page(t->pagedir, entry->upage);
	if (kpage == NULL) {
		/* This page was mapped but never faulted. */
		goto done_with_fault_cleanup;
	}

	if (entry->type == PAGE_FILE_BACKED &&
	    pagedir_is_dirty(t->pagedir, entry->upage)) {
		ASSERT(entry->rw == PAGE_WRITABLE);
		struct file *file = fd_to_file(entry->file.fd);
		ASSERT(file != NULL);
		acquire_io_lock();
		file_seek(file, entry->file.pos);
		const off_t eof = file_length(file);
		const off_t bytes = file_write(file, kpage, PGSIZE);
		ASSERT(bytes == PGSIZE || bytes + entry->file.pos == eof);
		release_io_lock();
	}

	if (swap_anonymous_memory && /* Caller opts-in to swap. */
	    entry->type == PAGE_ANONYMOUS &&
	    pagedir_is_accessed(t->pagedir, entry->upage)) {
		struct swap_slot s = swap_save(kpage);
		if (swap_slot_is_valid(s)) {
			entry->anon.swap = s;
		} else {
			swap_slot_unset(&entry->anon.swap);
		}
	}

	pagedir_clear_page(t->pagedir, entry->upage);
	palloc_free_page(kpage);

done_with_fault_cleanup:
	// TODO: free allocated page_entry; currently seems to cause
	// weird page faults and crashes; not sure why
	// free(entry);
}

static void
page_destructor(struct hash_elem *e_, void *aux UNUSED)
{
	page_evict_prepare(e_, /* swap_anonymous_memory */ false);
}

void
page_init(void)
{
	struct thread *t = thread_current();
	lock_acquire(&t->vm.lock);
	ASSERT(!t->vm.initialized);
	hash_init(&t->vm.page_table, page_hash, page_less, NULL);
	t->vm.initialized = true;
	lock_release(&t->vm.lock);
}

void
page_destroy(void)
{
	struct thread *t = thread_current();
	lock_acquire(&t->vm.lock);
	ASSERT(t->vm.initialized);
	hash_destroy(&t->vm.page_table, page_destructor);
	lock_release(&t->vm.lock);
}

static bool
is_stack_access(struct intr_frame *f, void *uaddr)
{
	ASSERT(is_user_vaddr(uaddr));
	if (f == NULL) {
		return NULL;
	}

	/* Per Pintos Project 3 guidance:
	 *
	 * User programs are buggy if they write to the stack below the stack
	 * pointer [...] However, the 80x86 PUSH instruction checks access
	 * permissions before it adjusts the stack pointer, so it may cause a
	 * page fault 4 bytes below the stack pointer [...] Similarly, the
	 * PUSHA instruction pushes 32 bytes at once, so it can fault 32 bytes
	 * below the stack pointer. */
	if (uaddr < f->esp - 32) {
		return false;
	}

	const size_t sz = PHYS_BASE - uaddr;
	if (sz > 8 * 1024 * 1024) { /* 8MB stack limit */
		return false;
	}

	return true;
}

static struct page_entry *
page_map_common(enum palloc_flags extra_flags, void *upage, enum page_rw rw)
{
	struct thread *t = thread_current();
	ASSERT(t->vm.initialized);
	ASSERT(lock_held_by_current_thread(&t->vm.lock));

	if (upage == NULL) {
		/* Reject mapping at address zero. */
		return NULL;
	}

	if (pg_round_down(upage) != upage) {
		/* Reject any uaddr not aligned on a page boundary. */
		return NULL;
	}

	struct hash_elem *collision = NULL;
	{
		struct page_entry key = {
			.upage = upage,
		};
		collision = hash_find(&t->vm.page_table, &key.elem);
	}
	if (collision != NULL) {
		/* Reject any uaddr that collides with an existing mapping. */
		return NULL;
	}

	struct page_entry *entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}

	entry->upage = upage;
	entry->flags = extra_flags;
	entry->rw = rw;
	entry->type = PAGE_ANONYMOUS;
	swap_slot_unset(&entry->anon.swap);
	struct hash_elem *e = hash_insert(&t->vm.page_table, &entry->elem);
	ASSERT(e == NULL); /* Guaranteed by earlier hash_find(). */
	return entry;
}

static bool
page_fault_impl(struct intr_frame *f, void *uaddr, void **kpage_out)
{
	struct page_entry key = {
		.upage = pg_round_down(uaddr),
	};
	enum palloc_flags flags = 0;
	enum page_rw rw = PAGE_READONLY;
	enum page_type type = PAGE_ANONYMOUS;
	struct swap_slot swap = {0};
	int fd = FD_INVALID;
	off_t pos = 0;

	bool got_entry_fields = false;
	{
		struct thread *t = thread_current();
		lock_acquire(&t->vm.lock);
		ASSERT(t->vm.initialized);

		struct hash_elem *e = hash_find(&t->vm.page_table, &key.elem);
		struct page_entry *entry = NULL;
		if (e != NULL) {
			/* Found mapping registered by page_map_common(). */
			entry = hash_entry(e, struct page_entry, elem);
		} else if (is_stack_access(f, uaddr)) {
			/* Register new mapping to accomodate stack growth. */
			entry = page_map_common(PAL_ZERO,
			                        key.upage,
			                        PAGE_WRITABLE);
			ASSERT(entry->type == PAGE_ANONYMOUS);
		}

		if (entry != NULL) {
			got_entry_fields = true;
			flags = entry->flags;
			rw = entry->rw;
			type = entry->type;
			switch (entry->type) {
			case PAGE_ANONYMOUS:
				swap = entry->anon.swap;
				break;
			case PAGE_FILE_BACKED:
				fd = entry->file.fd;
				pos = entry->file.pos;
				break;
			}
		}

		lock_release(&t->vm.lock);
	}
	if (!got_entry_fields) {
		return false;
	}

	void *kpage = frame_get_page(key.upage, flags, rw);
	ASSERT(kpage != NULL);

	if (type == PAGE_ANONYMOUS) {
		if (swap_slot_is_valid(swap)) {
			swap_load(swap, kpage);
		}
	} else if (type == PAGE_FILE_BACKED) {
		struct file *file = fd_to_file(fd);
		ASSERT(file != NULL);
		ASSERT(intr_get_level() == INTR_ON);
		acquire_io_lock();
		const off_t to_restore = file_tell(file);
		file_seek(file, pos);
		const off_t bytes = file_read(file, kpage, PGSIZE);
		file_seek(file, to_restore);
		release_io_lock();
		if (bytes < PGSIZE) {
			memset(kpage + bytes, 0, PGSIZE - bytes);
		}
	} else {
		ASSERT(0 && "Invalid value for enum page_type");
	}

	if (kpage_out != NULL) {
		*kpage_out = kpage;
	}
	return true;
}

bool
page_fault_on(struct intr_frame *f, void *uaddr)
{
	return page_fault_impl(f, uaddr, NULL);
}

bool
page_map(int fd, off_t pos, void *upage, enum page_rw rw)
{
	ASSERT(pos % PGSIZE == 0); /* Must be page-aligned. */
	bool mapped = false;

	struct thread *t = thread_current();
	lock_acquire(&t->vm.lock);
	{
		struct page_entry *entry = page_map_common(0, upage, rw);
		if (entry != NULL) {
			struct file *file = fd_to_file(fd);
			ASSERT(file != NULL);

			entry->type = PAGE_FILE_BACKED;
			entry->file.mmap = MMAP_ID_UNSET;
			entry->file.fd = fd;
			entry->file.pos = pos;

			mapped = true;
		}
	}
	lock_release(&t->vm.lock);

	return mapped;
}

bool
page_map_zero(void *upage, enum page_rw rw)
{
	struct thread *t = thread_current();
	lock_acquire(&t->vm.lock);
	const bool mapped = (page_map_common(PAL_ZERO, upage, rw) != NULL);
	lock_release(&t->vm.lock);
	return mapped;
}

void *
page_create(enum palloc_flags extra_flags, void *upage, enum page_rw rw)
{
	struct thread *t = thread_current();
	lock_acquire(&t->vm.lock);
	const bool mapped = (page_map_common(extra_flags, upage, rw) != NULL);
	lock_release(&t->vm.lock);

	if (!mapped) {
		return NULL;
	}

	void *kpage = NULL;
	if (!page_fault_impl(NULL, upage, &kpage)) {
		// TODO: on error, free() entry instead of waiting for exit()
		return NULL;
	}

	return kpage;
}

struct page_descriptor
page_mmap(int fd, void *upage)
{
	struct page_descriptor pd = {
		.id = PAGE_DESCRIPTOR_ERROR,
	};
	struct file *reopened = NULL;
	off_t len = 0;

	acquire_io_lock();
	{
		struct file *file = fd_to_file(fd);
		reopened = (file == NULL) ? NULL : file_reopen(file);
		len = (reopened == NULL) ? 0 : file_length(reopened);
	}
	release_io_lock();

	if (reopened == NULL) {
		return pd;
	}

	const int fd_new = fd_register(reopened);
	struct thread *t = thread_current();
	pd.id = t->vm.mmap_generator++;

	for (off_t pos = 0; pos < len; pos += PGSIZE) {
		lock_acquire(&t->vm.lock);
		struct page_entry *entry =
			page_map_common(0, upage + pos, PAGE_WRITABLE);
		if (entry == NULL) {
			// TODO: unwind with page_munmap()
			lock_release(&t->vm.lock);
			pd.id = PAGE_DESCRIPTOR_ERROR; /* Unset on error. */
			break;
		}
		entry->type = PAGE_FILE_BACKED;
		entry->file.mmap = pd.id;
		entry->file.fd = fd_new;
		entry->file.pos = pos;
		lock_release(&t->vm.lock);
	}

	return pd;
}

void
page_munmap(struct page_descriptor pd)
{
	struct thread *t = thread_current();
	struct hash_iterator i = {0};
	int got_fd = FD_INVALID;

	struct list to_unmap;
	list_init(&to_unmap);

	lock_acquire(&t->vm.lock);

	hash_first(&i, &t->vm.page_table);
	while (hash_next(&i)) {
		struct page_entry *entry =
			hash_entry(hash_cur(&i), struct page_entry, elem);
		if (entry->type == PAGE_FILE_BACKED &&
		    entry->file.mmap == pd.id) {
			if (got_fd == FD_INVALID) {
				got_fd = entry->file.fd;
			} else {
				ASSERT(got_fd == entry->file.fd);
			}
			list_push_back(&to_unmap, &entry->munmap_elem);
		}
	}

	struct list_elem *e = list_begin(&to_unmap);
	for (; e != list_end(&to_unmap); e = list_next(e)) {
		struct page_entry *entry =
			list_entry(e, struct page_entry, munmap_elem);
		struct hash_elem *hashed =
			hash_delete(&t->vm.page_table, &entry->elem);
		ASSERT(hashed != NULL);
		page_evict_prepare(hashed, /* swap_anonymous_memory */ false);
	}

	lock_release(&t->vm.lock);

	struct file *file = (got_fd == FD_INVALID) ? NULL : fd_to_file(got_fd);
	if (file != NULL) {
		fd_unregister(got_fd);
		acquire_io_lock();
		file_close(file);
		release_io_lock();
	}
}

struct page_evict_args {
	tid_t owner;
	void *upage;
};

static void
page_evict_from_owner(struct thread *t, void *aux)
{
	struct page_evict_args *args = aux;
	if (t->tid != args->owner) {
		return;
	}

	// TODO: not safe to acquire locks and reenable interrupts inside
	// thread_foreach() callback; may lead to invalid all_list iterator?
	lock_acquire(&t->vm.lock);

	struct page_entry key = {
		.upage = args->upage,
	};
	struct hash_elem *found = hash_find(&t->vm.page_table, &key.elem);
	if (found != NULL) {
		page_evict_prepare(found, /* swap_anonymous_memory */ true);
	}

	lock_release(&t->vm.lock);
}

void
page_evict(tid_t owner, void *upage)
{
	struct page_evict_args args = {
		.owner = owner,
		.upage = upage,
	};
	thread_foreach(page_evict_from_owner, &args);
}
