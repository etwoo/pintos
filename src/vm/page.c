#include "vm/page.h"

#include "threads/thread.h"
#include "vm/frame.h"

struct page_entry {
	void *upage;
	enum palloc_flags flags;
	bool writable;
	struct hash_elem elem; /* Hash element for page_table. */
};

unsigned
page_hash(const struct hash_elem *e_, void *aux)
{
	const struct page_entry *e = hash_entry(e_, struct page_entry, elem);
	return pg_no(e->upage);
}

bool
page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
	const struct page_entry *a = hash_entry(a_, struct page_entry, elem);
	const struct page_entry *b = hash_entry(b_, struct page_entry, elem);
	return pg_no(a->upage) < pg_no(b->upage);
}

void
page_init(struct hash *page_table)
{
	hash_init(page_table, page_hash, page_less, NULL);
}

static void *
page_create_impl(enum palloc_flags extra_flags, void *upage, bool writable)
{
	struct thread *t = thread_current();

	struct page_entry *entry = malloc(sizeof(*pe));
	if (entry == NULL) {
		return NULL;
	}

	entry->upage = upage;
	entry->flags = extra_flags;
	entry->writable = writable;
	struct hash_elem *e = hash_insert(&t->vm.page_table, entry);
	ASSERT(e == NULL); // TODO: ok to assume dup upage is caller error?
	return kpage;
}

// TODO: free() page_entry values in hashtable (created above) on process_exit()

void *
page_create(void *upage, bool writable)
{
	return page_create_impl(0, upage, writable);
}

void *
page_create_zero(void *upage, bool writable)
{
	return page_create_impl(PAL_ZERO, upage, writable);
}

bool
page_fault_on(void *uaddr)
{
	struct thread *t = thread_current();
	struct page_entry key = {
		.upage = pg_round_down(uaddr),
		.writable = true,
	};
	void *kpage = NULL;

	struct hash_elem *e = hash_find(&t->vm.page_table, &key);
	if (e == NULL) {
		goto err;
	}

	struct page_entry *entry = hash_entry(e, struct page_entry, elem);
	ASSERT(entry != NULL);

	void *kpage = palloc_get_page(entry->flags | PAL_USER);
	if (kpage == NULL) {
		// TODO: evict? swap? pagedir_clear_page()?
		goto err;
	}

	ASSERT(pagedir_get_page(t->pagedir, key.upage) == NULL);
	if (!pagedir_set_page(t->pagedir, key.upage, kpage, entry->writable)) {
		goto err;
	}

	return true;

err:
	if (kpage != NULL) {
		palloc_free_page(kpage);
	}
	return false;
}
