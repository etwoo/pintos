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
	} state;
	block_sector_t sector;
	int64_t accessed_at;
	void *data[BLOCK_SECTOR_SIZE];
};

struct cache_request {
	block_sector_t sector;
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

		block_read(fs_device, r->sector, r->block->data);
		r->block->state = CACHE_POPULATED;

		cond_signal(&r->request_done, &fs_cache.lock);
	}

	lock_release(&fs_cache.lock);
}

void
cache_init(int64_t writeback_period_ms)
{
	ASSERT(fs_device != NULL);
	lock_init(&fs_cache.lock);
	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		cache_block_reset(fs_cache.blocks + i);
	}

	(void)writeback_period_ms; // TODO: spawn thread for periodic writeback
	// combine with reader thread? IOPS serialized at IDE layer anyway
	thread_create("cache-io", PRI_DEFAULT, cache_io_thread, NULL);
}

void
cache_done(void)
{
	// TODO: force synchronous writeback
}

static struct cache_block *
cache_find(block_sector_t sector)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->state == CACHE_POPULATED && b->sector == sector) {
			return b;
		}
	}

	// TODO: second chance or clock algo for cache eviction
	struct cache_block *oldest = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (oldest == NULL || b->accessed_at < oldest->accessed_at) {
			oldest = b;
		}
	}

	cache_block_reset(oldest);
	return oldest;
}

static void
cache_read_async(block_sector_t sector, struct cache_block *to_fill)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	struct cache_request r;
	r.sector = sector;
	r.block = to_fill;
	cond_init(&r.request_done);

	list_push_back(&fs_cache.requests, &r.elem);
	r.block->state = CACHE_READ_QUEUED;

	cond_signal(&fs_cache.requests_pending, &fs_cache.lock);

	while (r.block->state == CACHE_READ_QUEUED) {
		cond_wait(&r.request_done, &fs_cache.lock);
	}
}

bool
cache_read(block_sector_t sector, void *buffer, size_t bytes)
{
	ASSERT(bytes <= BLOCK_SECTOR_SIZE);
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
		/* Cache hit. Copy value to caller. */
		break;
	}

	if (cached != NULL) {
		success = true;
		ASSERT(cached->state == CACHE_POPULATED);
		memcpy(buffer, cached->data, bytes);
		cached->accessed_at = timer_ticks();
	}

	lock_release(&fs_cache.lock);
	return success;
}

void
cache_write(block_sector_t sector, const void *buffer)
{
	(void)sector; // TODO cache_write
	(void)buffer; // TODO cache_write
}
