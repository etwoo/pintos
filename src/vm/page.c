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

#include <string.h>

enum page_type {
	PAGE_ANONYMOUS,
	PAGE_FILE_BACKED,
};

struct page_entry {
	void *upage;
	enum palloc_flags flags;
	enum page_rw rw;
	enum page_type type;
	union {
		struct {
			int swap_slot; // TODO
		} anonymous;
		struct {
			int fd;
			off_t pos;
		} file;
	};
	struct hash_elem elem; /* Hash element for page_table. */
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

void
page_init(void)
{
	struct thread *t = thread_current();
	ASSERT(!t->vm.initialized);
	hash_init(&t->vm.page_table, page_hash, page_less, NULL);
	t->vm.initialized = true;
}

static bool
page_fault_impl(void *uaddr, void **kpage_out)
{
	void *kpage = NULL;

	struct thread *t = thread_current();
	ASSERT(t->vm.initialized);

	struct page_entry key = {
		.upage = pg_round_down(uaddr),
		.rw = PAGE_WRITABLE, // TODO rm, unused
	};
	struct hash_elem *e = hash_find(&t->vm.page_table, &key.elem);
	if (e == NULL) {
		goto err;
	}

	struct page_entry *entry = hash_entry(e, struct page_entry, elem);
	ASSERT(entry != NULL);

	kpage = palloc_get_page(entry->flags | PAL_USER);
	if (kpage == NULL) {
		// TODO: evict? swap? pagedir_clear_page()?
		goto err;
	}

	if (entry->type == PAGE_FILE_BACKED) {
		struct file *file = fd_to_file(entry->file.fd);
		// TODO: what if page fault for mapping set by page_map() only
		// happens after fd is closed? since mapping already happened,
		// fault should work, but deferred fd_to_file() in fault handler
		// will fail, hit ASSERT below; maybe give fd a reference count?
		ASSERT(file != NULL);
		ASSERT(intr_get_level() == INTR_ON);
		acquire_io_lock();
		const off_t to_restore = file_tell(file);
		file_seek(file, entry->file.pos);
		const off_t bytes = file_read(file, kpage, PGSIZE);
		file_seek(file, to_restore);
		release_io_lock();
		if (bytes < PGSIZE) {
			memset(kpage + bytes, 0, PGSIZE - bytes);
		}
	}

	ASSERT(pagedir_get_page(t->pagedir, key.upage) == NULL);
	const bool writable = (entry->rw == PAGE_WRITABLE);
	if (!pagedir_set_page(t->pagedir, key.upage, kpage, writable)) {
		goto err;
	}

	if (kpage_out != NULL) {
		*kpage_out = kpage;
	}
	return true;

err:
	if (kpage != NULL) {
		palloc_free_page(kpage);
	}
	return false;
}

bool
page_fault_on(void *uaddr)
{
	return page_fault_impl(uaddr, NULL);
}

// TODO: free() page_entry values in hashtable (malloc below) on process_exit()
static struct page_entry *
page_map_common(enum palloc_flags extra_flags, void *upage, enum page_rw rw)
{
	ASSERT(pg_round_down(upage) == upage); /* Must be page-aligned. */

	struct thread *t = thread_current();
	ASSERT(t->vm.initialized);

	struct page_entry *entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}

	entry->upage = upage;
	entry->flags = extra_flags;
	entry->rw = rw;
	entry->type = PAGE_ANONYMOUS;
	struct hash_elem *e = hash_insert(&t->vm.page_table, &entry->elem);
	ASSERT(e == NULL); // TODO: handle duplicate or overlapping upage
	return entry;
}

bool
page_map(int fd, off_t pos, void *upage, enum page_rw rw)
{
	ASSERT(pos % PGSIZE == 0); /* Must be page-aligned. */

	struct page_entry *entry = page_map_common(0, upage, rw);
	if (entry == NULL) {
		return false;
	}

	struct file *file = fd_to_file(fd);
	ASSERT(file != NULL);

	entry->type = PAGE_FILE_BACKED;
	entry->file.fd = fd;
	entry->file.pos = pos;
	return true;
}

bool
page_map_zero(void *upage, enum page_rw rw)
{
	struct page_entry *entry = page_map_common(PAL_ZERO, upage, rw);
	return (entry != NULL);
}

void *
page_create(enum palloc_flags extra_flags, void *upage, enum page_rw rw)
{
	struct page_entry *entry = page_map_common(extra_flags, upage, rw);
	if (entry == NULL) {
		return NULL;
	}

	void *kpage = NULL;
	if (!page_fault_impl(upage, &kpage)) {
		// TODO: on error, free() entry instead of waiting for exit()
		return NULL;
	}

	return kpage;
}
