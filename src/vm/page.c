#include "vm/page.h"

#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"

struct page_entry {
	void *upage;
	enum palloc_flags flags;
	bool writable;
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
		.writable = true,
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

	ASSERT(pagedir_get_page(t->pagedir, key.upage) == NULL);
	if (!pagedir_set_page(t->pagedir, key.upage, kpage, entry->writable)) {
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

// TODO: reuse page_create_lazy() for mmap()
static bool
page_create_lazy(enum palloc_flags extra_flags, void *upage, bool writable)
{
	struct thread *t = thread_current();
	ASSERT(t->vm.initialized);

	struct page_entry *entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		return false;
	}

	entry->upage = upage;
	entry->flags = extra_flags;
	entry->writable = writable;
	struct hash_elem *e = hash_insert(&t->vm.page_table, &entry->elem);
	ASSERT(e == NULL); // TODO: ok to assume dup upage is caller error?
	return true;
}

// TODO: free() page_entry values in hashtable (created above) on process_exit()

static void *
page_create_eager_impl(enum palloc_flags extra_flags,
                       void *upage,
                       bool writable)
{
	void *kpage = NULL;
	if (page_create_lazy(extra_flags, upage, writable) &&
	    page_fault_impl(upage, &kpage)) {
		return kpage;
	}
	return NULL;
}

void *
page_create_eager(void *upage, bool writable)
{
	return page_create_eager_impl(0, upage, writable);
}

void *
page_create_eager_zero(void *upage, bool writable)
{
	return page_create_eager_impl(PAL_ZERO, upage, writable);
}
