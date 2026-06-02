#include "vm/frame.h"

#include "threads/synch.h"

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

struct frame_table_entry {
	void *uaddr;
	uintptr_t *pte; // TODO: ptov(*pte) == kaddr, for reverse-mapping
	int atime;      /* Approximate last access time, in timer ticks. */
	int pinned;     // TODO: reference count for pinning uaddr/frame
	struct list elem;
};

// TODO: insert new value, with last_access set to sentinel, like INT_MIN
// TODO: evict entry with earliest (minimum) last_access
//
// .... but note that atime is a lower bound
// accordingly, check if page has PTE_A, and if so, update+clear PTE_A
// (pop+push onto list to maintain sort?) and update atime (estimate), then
// keep looking for next page with low atime basically, seek til we find lowest
// "clean" atime (no PTE_A bit set)
//
// TODO: touch existing value -- pop, update_last access, push, clear PTE_A ..?
// TODO: if touch existing value through kaddr, use frame.uaddr to get reverse
//       mapping and update access bit for user alias as well
struct frame_table {
	struct lock lock;
	struct list table;
	struct {
		struct hash by_uaddr; /* Lookup by user address of frame.   */
		struct hash by_kaddr; /* Lookup by kernel address of frame. */
	} aliasing;
};

static struct frame_table ft; /* frames that contain a user page. */

void
frame_init(void)
{
	lock_init(&ft.lock);
	list_init(&ft.table);
}
