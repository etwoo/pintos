#include "filesys/cache.h"

#include "devices/timer.h"
#include "filesys/filesys.h"
#include "filesys/inode_disk.h" /* for INODE_SECTOR_UNSET */
#include "threads/malloc.h"
#include "threads/thread.h"

#include <array.h>
#include <debug.h>
#include <string.h>

static const block_sector_t CACHE_SECTOR_UNSET = UINT32_MAX;

enum cache_block_state {
	CACHE_UNUSED,
	CACHE_IO_QUEUED,
	CACHE_IO_AWAIT_FIRST_USE,
	CACHE_CLEAN,
	CACHE_DIRTY,
};

struct cache_block {
	enum cache_block_state state;
	block_sector_t sector;
	int64_t accessed_at;
	char data[BLOCK_SECTOR_SIZE];
	struct {
		int awaiting;
		struct condition ready;
		enum cache_block_state ready_state;
	} io_async;
};

enum cache_request_op {
	REQUEST_READ,
	REQUEST_WRITE,
	REQUEST_DRAIN_AND_TEARDOWN,
};

struct cache_request {
	enum cache_request_op op;
	block_sector_t sector_extra;
	struct cache_block *block;
	struct list_elem elem;
};

struct cache {
	struct lock lock;
	struct cache_block blocks[64];
	struct list requests;
	struct condition requests_pending;
};

static struct cache fs_cache;

static void
cache_block_reset(struct cache_block *b)
{
	ASSERT(b->state == CACHE_UNUSED || b->state == CACHE_CLEAN);
	ASSERT(b->io_async.awaiting == 0);

	b->state = CACHE_UNUSED;
	b->sector = CACHE_SECTOR_UNSET;
	b->accessed_at = INT64_MIN;
	memset(b->data, 0, sizeof(b->data));
	cond_init(&b->io_async.ready);
	b->io_async.ready_state = CACHE_UNUSED;
}

static void
cache_io_thread(void *aux UNUSED)
{
	lock_acquire(&fs_cache.lock);

	while (true) {
		while (list_empty(&fs_cache.requests)) {
			cond_wait(&fs_cache.requests_pending, &fs_cache.lock);
		}

		struct list_elem *e = list_pop_front(&fs_cache.requests);
		struct cache_request *r =
			list_entry(e, struct cache_request, elem);
		ASSERT(r->block->state == CACHE_IO_QUEUED);

		/* Choose target sector (before releasing cache lock). */
		block_sector_t sector = r->block->sector;
		if (r->op == REQUEST_DRAIN_AND_TEARDOWN) {
			ASSERT(r->block->sector == CACHE_SECTOR_UNSET);
			ASSERT(r->sector_extra != CACHE_SECTOR_UNSET);
			/* Drain and teardown requests store target sector
			   out-of-band. Concurrent cache lookups thus see a
			   cache_block with sector == CACHE_SECTOR_UNSET and do
			   not interpret as a potential cache hit. In other
			   words, unsetting cache_block.sector denies new I/O
			   requests against this cache block, ensuring forward
			   progress on drain and teardown. */
			sector = r->sector_extra;
		}

		/* Release cache lock while performing block I/O to avoid
		   blocking concurrent cache lookups that can otherwise be
		   satisfied purely in-memory. */
		lock_release(&fs_cache.lock);

		switch (r->op) {
		case REQUEST_READ:
			block_read(fs_device, sector, r->block->data);
			break;
		case REQUEST_WRITE:
		case REQUEST_DRAIN_AND_TEARDOWN:
			block_write(fs_device, sector, r->block->data);
			break;
		}

		lock_acquire(&fs_cache.lock);

		if (r->op == REQUEST_DRAIN_AND_TEARDOWN) {
			ASSERT(r->block->sector == CACHE_SECTOR_UNSET);
		} else {
			/* Verify buffer was not reused behind our backs. */
			ASSERT(r->block->sector == sector);
		}

		r->block->state = r->block->io_async.ready_state;
		if (r->block->state == CACHE_IO_AWAIT_FIRST_USE) {
			/* Awaiting caller requested to keep this cache_block
			   alive (i.e. prevent eviction) until at least one
			   read from the cache can complete. */
			r->block->io_async.awaiting++;
		}
		r->block->io_async.ready_state = CACHE_UNUSED;

		cond_broadcast(&r->block->io_async.ready, &fs_cache.lock);

		free(r); /* Originally allocated by cache_io_async(). */
	}

	lock_release(&fs_cache.lock);
}

static void
cache_writeback_thread(void *aux UNUSED)
{
	while (true) {
		timer_msleep(5 * 1000); /* 5 seconds */
		cache_done();
	}
}

void
cache_init()
{
	ASSERT(fs_device != NULL);

	lock_init(&fs_cache.lock);
	list_init(&fs_cache.requests);
	cond_init(&fs_cache.requests_pending);

	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		cache_block_reset(fs_cache.blocks + i);
	}

	thread_create("cache-io", PRI_DEFAULT, cache_io_thread, NULL);
	thread_create("writeback", PRI_DEFAULT, cache_writeback_thread, NULL);
}

static bool
cache_io_async(enum cache_request_op op,
               struct cache_block *cache,
               block_sector_t sector_extra,
               enum cache_block_state ready_state,
               bool wait_until_ready)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	struct cache_request *r = malloc(sizeof(*r));
	if (r == NULL) {
		return false;
	}

	cache->state = CACHE_IO_QUEUED;
	if (sector_extra != CACHE_SECTOR_UNSET) {
		ASSERT(cache->sector == CACHE_SECTOR_UNSET);
		ASSERT(sector_extra < block_size(fs_device));
	} else {
		ASSERT(cache->sector != CACHE_SECTOR_UNSET);
		ASSERT(cache->sector < block_size(fs_device));
	}

	cache->io_async.ready_state = ready_state;

	r->op = op;
	r->sector_extra = sector_extra;
	r->block = cache;
	list_push_back(&fs_cache.requests, &r->elem);

	/* cache_io_thread() takes ownership of cache_request memory. */
	r = NULL;

	cond_signal(&fs_cache.requests_pending, &fs_cache.lock);

	while (wait_until_ready && cache->state == CACHE_IO_QUEUED) {
		cond_wait(&cache->io_async.ready, &fs_cache.lock);
	}
	return true;
}

static bool
cache_read_async(block_sector_t sector,
                 struct cache_block *to_fill,
                 enum cache_block_state ready_state)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	to_fill->sector = sector;
	return cache_io_async(REQUEST_READ,
	                      to_fill,
	                      CACHE_SECTOR_UNSET,
	                      ready_state,
	                      /* wait_until_ready */ true);
}

static bool
cache_prepare_drain_teardown_async(struct cache_block *to_flush)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	const block_sector_t sector_extra = to_flush->sector;

	/* Deny new IO for this block. Caller may now cache_block_drain(). */
	to_flush->sector = CACHE_SECTOR_UNSET;

	return cache_io_async(REQUEST_DRAIN_AND_TEARDOWN,
	                      to_flush,
	                      sector_extra,
	                      CACHE_IO_AWAIT_FIRST_USE,
	                      /* wait_until_ready */ true);
}

static bool
cache_flush_async(struct cache_block *to_flush)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	return cache_io_async(REQUEST_WRITE,
	                      to_flush,
	                      CACHE_SECTOR_UNSET,
	                      CACHE_CLEAN,
	                      /* wait_until_ready */ true);
}

static void
cache_optional_readahead(block_sector_t hint)
{
	return; // TODO: reenable readahead

	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	if (hint == INODE_SECTOR_UNSET) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->sector == hint) {
			/* Cache already contains entry for readahead hint.
			   Even a cache entry not ready to serve reads, like
			   one in state CACHE_IO_QUEUED, means dispatching a
			   new read request would be counterproductive. */
			return;
		}
	}

	struct cache_block *oldest = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (oldest == NULL || b->accessed_at < oldest->accessed_at) {
			switch (b->state) {
			case CACHE_UNUSED:
			case CACHE_CLEAN:
				// TODO: fix readahead logic re: draining,
				// maybe check b->sector == INODE_SECTOR_UNSET?
				// or only check awaiting, not list_empty()?
				if (list_empty(&b->io_async.ready.waiters) &&
				    b->io_async.awaiting == 0) {
					oldest = b;
				}
				break;
			case CACHE_DIRTY:
				/* Do not trigger writeback. */
			case CACHE_IO_QUEUED:
			case CACHE_IO_AWAIT_FIRST_USE:
				/* Do not evict entry under active use. */
				break;
			}
		}
	}

	if (oldest == NULL) {
		/* No cache space eligible for readahead. */
		return;
	}

	cache_block_reset(oldest);
	cache_read_async(hint, oldest, CACHE_CLEAN);
	/* Do not wait for request to complete. Also ignore failure. */
}

// TODO: reconsider parameters here, state transition logic more generally
static void
cache_block_drop_reference(struct cache_block *b,
                           enum cache_block_state state,
                           enum cache_block_state ready_state)
{
	ASSERT(b->io_async.awaiting > 0);
	if (--b->io_async.awaiting == 0) {
		b->state = state;
		b->io_async.ready_state = ready_state;
		cond_signal(&b->io_async.ready, &fs_cache.lock);
	}
}

static void
cache_block_drain(struct cache_block *b, bool add_reference)
{
	const block_sector_t sector_start = b->sector;

	if (add_reference) {
		b->io_async.ready_state = CACHE_IO_AWAIT_FIRST_USE;
		b->io_async.awaiting++;
		while (b->state == CACHE_IO_QUEUED) {
			cond_wait(&b->io_async.ready, &fs_cache.lock);
		}
	}

	cache_block_drop_reference(b, b->io_async.ready_state, CACHE_UNUSED);

	while (b->state == CACHE_IO_QUEUED || b->io_async.awaiting > 0) {
		cond_wait(&b->io_async.ready, &fs_cache.lock);
	}

	/* Verify buffer was not reused behind our backs. */
	ASSERT(b->sector == sector_start || b->sector == CACHE_SECTOR_UNSET);
}

static bool
cache_find(block_sector_t sector, struct cache_block **cached)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->sector == sector) {
			switch (b->state) {
			case CACHE_UNUSED:
				/* Unused cache entries should always have
				   sector unset. Ignore for now. */
				break;
			case CACHE_IO_QUEUED:
				/* Wait for in-flight data to be ready. */
				cache_block_drain(b, true);
				*cached = b;
				return true;
			case CACHE_IO_AWAIT_FIRST_USE:
			case CACHE_CLEAN:
			case CACHE_DIRTY:
				/* Cache hit. Return in-memory data. */
				*cached = b;
				return true;
			}
		}
	}

	/* LRU buffer cache replacement: use precise per-buffer access data in
	   cache_block.accessed_at, set by cache_read() and cache_write() after
	   each invocation of cache_find(). */
	struct cache_block *oldest = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (oldest == NULL || b->accessed_at < oldest->accessed_at) {
			switch (b->state) {
			case CACHE_UNUSED:
			case CACHE_CLEAN:
			case CACHE_DIRTY:
				// TODO: fix eviction logic re: draining,
				// maybe check b->sector == INODE_SECTOR_UNSET?
				// or only check awaiting, not list_empty()?
				if (list_empty(&b->io_async.ready.waiters) &&
				    b->io_async.awaiting == 0) {
					oldest = b;
				}
				break;
			case CACHE_IO_QUEUED:
			case CACHE_IO_AWAIT_FIRST_USE:
				/* Do not evict entry under active use. */
				break;
			}
		}
	}
	ASSERT(oldest != NULL);

	if (oldest->state == CACHE_DIRTY) {
		if (!cache_prepare_drain_teardown_async(oldest)) {
			return false;
		}
		cache_block_drain(oldest, false);
	}

	cache_block_reset(oldest);
	*cached = oldest;
	return true;
}

static void
assert_sector_pos_sz_in_range(int pos, int sz)
{
	ASSERT(0 <= pos && pos <= BLOCK_SECTOR_SIZE);
	ASSERT(0 <= sz && sz <= BLOCK_SECTOR_SIZE);
	ASSERT(pos + sz <= BLOCK_SECTOR_SIZE);
}

bool
cache_read(block_sector_t sector, int pos, int sz, void *buffer)
{
	const block_sector_t no_readahead = INODE_SECTOR_UNSET;
	return cache_read_with_readhead(sector, pos, sz, buffer, no_readahead);
}

bool
cache_read_with_readhead(block_sector_t sector,
                         int pos,
                         int sz,
                         void *buffer,
                         block_sector_t hint)
{
	lock_acquire(&fs_cache.lock);

	struct cache_block *cached = NULL;
	if (!cache_find(sector, &cached)) {
		goto done;
	}
	ASSERT(cached != NULL);

	bool got_reference = false;
	switch (cached->state) {
	case CACHE_UNUSED:
		/* Cache miss. Populate assigned cache entry. */
		if (!cache_read_async(sector,
		                      cached,
		                      CACHE_IO_AWAIT_FIRST_USE)) {
			goto done;
		}
		got_reference = true;
		break;
	case CACHE_CLEAN:
	case CACHE_DIRTY:
	case CACHE_IO_AWAIT_FIRST_USE:
		/* Cache hit. Copy value to caller. */
		break;
	case CACHE_IO_QUEUED:
		/* cache_find() should never return entries in this state. */
		NOT_REACHED();
		break;
	}

	ASSERT(cached->sector == sector);
	assert_sector_pos_sz_in_range(pos, sz);
	memcpy(buffer, cached->data + pos, sz);
	cached->accessed_at = timer_ticks();

	if (got_reference) {
		cache_block_drop_reference(cached, CACHE_CLEAN, CACHE_UNUSED);
	}
	cache_optional_readahead(hint);

done:
	lock_release(&fs_cache.lock);
	return true;
}

bool
cache_write(block_sector_t sector, int pos, int sz, const void *buffer)
{
	lock_acquire(&fs_cache.lock);

	struct cache_block *cached = NULL;
	if (!cache_find(sector, &cached)) {
		goto done;
	}
	ASSERT(cached != NULL);

	bool got_reference = false;
	switch (cached->state) {
	case CACHE_UNUSED:
		if (sz < BLOCK_SECTOR_SIZE) {
			/* Cache miss on partial write. Read existing values
			 * surrounding the target [pos, pos+sz] range, which
			 * the caller expects to remain unchanged. */
			if (!cache_read_async(sector,
			                      cached,
			                      CACHE_IO_AWAIT_FIRST_USE)) {
				goto done;
			}
			got_reference = true;
		} else {
			cached->sector = sector;
		}
		break;
	case CACHE_CLEAN:
	case CACHE_DIRTY:
	case CACHE_IO_AWAIT_FIRST_USE:
		/* Cache hit. Update existing value. */
		break;
	case CACHE_IO_QUEUED:
		/* cache_find() should never return entries in this state. */
		NOT_REACHED();
		break;
	}

	ASSERT(cached->sector == sector);
	assert_sector_pos_sz_in_range(pos, sz);
	memcpy(cached->data + pos, buffer, sz);
	cached->accessed_at = timer_ticks();

	if (got_reference) {
		cache_block_drop_reference(cached, CACHE_DIRTY, CACHE_UNUSED);
	} else {
		cached->state = CACHE_DIRTY;
	}

done:
	lock_release(&fs_cache.lock);
	return true;
}

void
cache_done(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		lock_acquire(&fs_cache.lock);
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->state == CACHE_DIRTY) {
			cache_flush_async(b);
		}
		lock_release(&fs_cache.lock);
	}
}
