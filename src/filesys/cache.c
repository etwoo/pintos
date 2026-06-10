#include "filesys/cache.h"

#include "devices/timer.h"
#include "filesys/filesys.h"
#include "threads/thread.h"

#include <array.h>
#include <debug.h>
#include <string.h>

#define SECTOR_UNSET UINT32_MAX

struct cache_block {
	enum {
		CACHE_UNUSED,
		CACHE_READ_QUEUED,
		CACHE_POPULATED,
		CACHE_DIRTY,
	} state;
	block_sector_t sector;
	int64_t accessed_at;
	char data[BLOCK_SECTOR_SIZE];
};

struct cache_request {
	struct cache_block *block;
	struct condition request_done;
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
	b->state = CACHE_UNUSED;
	b->sector = SECTOR_UNSET;
	b->accessed_at = INT64_MIN;
	// TODO: memset data[] to zero for debuggability?
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
		ASSERT(r->block->state == CACHE_READ_QUEUED);

		block_read(fs_device, r->block->sector, r->block->data);
		r->block->state = CACHE_POPULATED;

		cond_signal(&r->request_done, &fs_cache.lock);
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

static void
cache_flush_dirty(struct cache_block *b)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	ASSERT(b->sector != SECTOR_UNSET);
	block_write(fs_device, b->sector, b->data);

	ASSERT(b->state == CACHE_DIRTY);
	b->state = CACHE_POPULATED;
}

void
cache_done(void)
{
	lock_acquire(&fs_cache.lock);

	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->state == CACHE_DIRTY) {
			cache_flush_dirty(b);
		}
	}

	lock_release(&fs_cache.lock);
}

static struct cache_block *
cache_find(block_sector_t sector)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->sector == sector) {
			switch (b->state) {
			case CACHE_UNUSED:
			case CACHE_READ_QUEUED:
				/* Sector matches, but data is not ready. */
				break;
			case CACHE_POPULATED:
			case CACHE_DIRTY:
				/* Cache hit. Return in-memory data. */
				return b;
			}
		}
	}

	// TODO: second chance or clock algo for cache eviction
	struct cache_block *oldest = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (oldest == NULL || b->accessed_at < oldest->accessed_at) {
			switch (b->state) {
			case CACHE_UNUSED:
			case CACHE_POPULATED:
			case CACHE_DIRTY:
				oldest = b;
				break;
			case CACHE_READ_QUEUED:
				/* Do not evict entry under active use. */
				break;
			}
		}
	}

	ASSERT(oldest != NULL);
	if (oldest->state == CACHE_DIRTY) {
		cache_flush_dirty(oldest);
	}

	cache_block_reset(oldest);
	return oldest;
}

static void
cache_read_async(block_sector_t sector, struct cache_block *to_fill)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	struct cache_request r;
	cond_init(&r.request_done);

	r.block = to_fill;
	r.block->state = CACHE_READ_QUEUED;
	r.block->sector = sector;

	list_push_back(&fs_cache.requests, &r.elem);

	cond_signal(&fs_cache.requests_pending, &fs_cache.lock);

	while (r.block->state == CACHE_READ_QUEUED) {
		cond_wait(&r.request_done, &fs_cache.lock);
	}
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
	bool success = false;
	lock_acquire(&fs_cache.lock);

	struct cache_block *cached = cache_find(sector);
	switch (cached->state) {
	case CACHE_UNUSED:
		/* Cache miss. Populate assigned cache entry. */
		cache_read_async(sector, cached);
		break;
	case CACHE_READ_QUEUED:
		/* All cache slots are busy with in-flight reads. A smarter
		 * implementation would probably wait, but I don't expect this
		 * to happen in practice, especially given all IOPS currently
		 * serialize on a single per-disk lock. Fail fast here, and let
		 * callers handle fallible I/O. */
		cached = NULL;
		break;
	case CACHE_POPULATED:
	case CACHE_DIRTY:
		/* Cache hit. Copy value to caller. */
		break;
	}

	if (cached != NULL) {
		success = true;
		ASSERT(cached->sector == sector);
		assert_sector_pos_sz_in_range(pos, sz);
		memcpy(buffer, cached->data + pos, sz);
		cached->accessed_at = timer_ticks();
	}

	lock_release(&fs_cache.lock);
	return success;
}

bool
cache_write(block_sector_t sector, int pos, int sz, const void *buffer)
{
	bool success = false;
	lock_acquire(&fs_cache.lock);

	struct cache_block *cached = cache_find(sector);
	switch (cached->state) {
	case CACHE_UNUSED:
	case CACHE_POPULATED:
	case CACHE_DIRTY:
		break;
	case CACHE_READ_QUEUED:
		// TODO: wait behind existing queued read; if this means
		// multiple threads can wait on the same request, change
		// cond_signal() to cond_broadcast() in cache_read_async()
		ASSERT(false);
		cached = NULL;
		break;
	}

	if (cached != NULL) {
		success = true;
		if (cached->state == CACHE_UNUSED && sz < BLOCK_SECTOR_SIZE) {
			cache_read_async(sector, cached);
		}
		cached->state = CACHE_DIRTY;
		cached->sector = sector;
		assert_sector_pos_sz_in_range(pos, sz);
		memcpy(cached->data + pos, buffer, sz);
		cached->accessed_at = timer_ticks();
	}

	lock_release(&fs_cache.lock);
	return success;
}
