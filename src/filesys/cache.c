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
	CACHE_READ_QUEUED,
	CACHE_READ_AWAIT_FIRST_USE,
	CACHE_CLEAN,
	CACHE_DIRTY,
};

struct cache_block {
	enum cache_block_state state;
	block_sector_t sector;
	int64_t accessed_at;
	char data[BLOCK_SECTOR_SIZE];
	struct condition data_ready;
	enum cache_block_state data_ready_state;
};

struct cache_request {
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
	b->state = CACHE_UNUSED;
	b->sector = CACHE_SECTOR_UNSET;
	b->accessed_at = INT64_MIN;
	memset(b->data, 0, sizeof(b->data));
	cond_init(&b->data_ready);
	b->data_ready_state = CACHE_UNUSED;
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
		r->block->state = r->block->data_ready_state;
		r->block->data_ready_state = CACHE_UNUSED;

		cond_broadcast(&r->block->data_ready, &fs_cache.lock);

		free(r); /* Originally allocated by cache_read_async(). */
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

	ASSERT(b->sector != CACHE_SECTOR_UNSET);
	block_write(fs_device, b->sector, b->data);

	ASSERT(b->state == CACHE_DIRTY);
	b->state = CACHE_CLEAN;
}

void
cache_done(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		lock_acquire(&fs_cache.lock);
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->state == CACHE_DIRTY) {
			cache_flush_dirty(b);
		}
		lock_release(&fs_cache.lock);
	}
}

enum read_wait_mode {
	WAIT_FOR_DATA,
	RETURN_AFTER_ENQUEUE,
};

static bool
cache_read_async(block_sector_t sector,
                 struct cache_block *to_fill,
                 enum read_wait_mode mode)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	ASSERT(sector < block_size(fs_device));

	struct cache_request *r = malloc(sizeof(*r));
	if (r == NULL) {
		return false;
	}

	to_fill->state = CACHE_READ_QUEUED;
	to_fill->sector = sector;

	switch (mode) {
	case WAIT_FOR_DATA:
		/* Caller waits synchronously and reads at least once. */
		to_fill->data_ready_state = CACHE_READ_AWAIT_FIRST_USE;
		break;
	case RETURN_AFTER_ENQUEUE:
		/* Caller returns without reading result. */
		to_fill->data_ready_state = CACHE_CLEAN;
		break;
	}

	r->block = to_fill;
	list_push_back(&fs_cache.requests, &r->elem);

	/* cache_io_thread() takes ownership of cache_request memory. */
	r = NULL;

	cond_signal(&fs_cache.requests_pending, &fs_cache.lock);

	switch (mode) {
	case WAIT_FOR_DATA:
		while (to_fill->state == CACHE_READ_QUEUED) {
			cond_wait(&to_fill->data_ready, &fs_cache.lock);
		}
		ASSERT(to_fill->state == CACHE_READ_AWAIT_FIRST_USE);
		break;
	case RETURN_AFTER_ENQUEUE:
		break;
	}

	return true;
}

static void
cache_optional_readahead(block_sector_t hint)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));

	if (hint == INODE_SECTOR_UNSET) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(fs_cache.blocks); ++i) {
		struct cache_block *b = &fs_cache.blocks[i];
		if (b->sector == hint) {
			/* Cache already contains entry for readahead hint.
			   Even a cache entry not ready to serve reads, like
			   one in state CACHE_READ_QUEUED, means dispatching
			   a new read request would be counterproductive. */
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
				oldest = b;
				break;
			case CACHE_DIRTY:
				/* Do not trigger writeback. */
			case CACHE_READ_QUEUED:
			case CACHE_READ_AWAIT_FIRST_USE:
				/* Do not evict entry under active use. */
				break;
			}
		}
	}

	if (oldest == NULL) {
		/* No cache space eligible for readahead. */
		return;
	}

	ASSERT(oldest->state == CACHE_UNUSED || oldest->state == CACHE_CLEAN);
	cache_block_reset(oldest);

	cache_read_async(hint, oldest, RETURN_AFTER_ENQUEUE);
	/* Do not wait for request to complete. Also ignore failure. */
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
				/* Unused cache entries should always have
				   sector unset. Ignore for now. */
				break;
			case CACHE_READ_QUEUED:
				/* Wait for in-flight data to be ready. */
				while (b->state == CACHE_READ_QUEUED) {
					cond_wait(&b->data_ready,
					          &fs_cache.lock);
				}
				__attribute__((fallthrough));
			case CACHE_READ_AWAIT_FIRST_USE:
			case CACHE_CLEAN:
			case CACHE_DIRTY:
				/* Cache hit. Return in-memory data. */
				return b;
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
				oldest = b;
				break;
			case CACHE_READ_QUEUED:
			case CACHE_READ_AWAIT_FIRST_USE:
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

	struct cache_block *cached = cache_find(sector);
	ASSERT(cached != NULL);

	switch (cached->state) {
	case CACHE_UNUSED:
		/* Cache miss. Populate assigned cache entry. */
		if (!cache_read_async(sector, cached, WAIT_FOR_DATA)) {
			goto done;
		}
		ASSERT(cached->state == CACHE_READ_AWAIT_FIRST_USE);
		cached->state = CACHE_CLEAN;
		break;
	case CACHE_CLEAN:
	case CACHE_DIRTY:
	case CACHE_READ_AWAIT_FIRST_USE:
		/* Cache hit. Copy value to caller. */
		break;
	case CACHE_READ_QUEUED:
		/* cache_find() should never return entries in this state. */
		NOT_REACHED();
		break;
	}

	ASSERT(cached->sector == sector);
	assert_sector_pos_sz_in_range(pos, sz);
	memcpy(buffer, cached->data + pos, sz);
	cached->accessed_at = timer_ticks();

	cache_optional_readahead(hint);

done:
	lock_release(&fs_cache.lock);
	return true;
}

bool
cache_write(block_sector_t sector, int pos, int sz, const void *buffer)
{
	lock_acquire(&fs_cache.lock);

	struct cache_block *cached = cache_find(sector);
	ASSERT(cached != NULL);

	switch (cached->state) {
	case CACHE_UNUSED:
		if (sz < BLOCK_SECTOR_SIZE) {
			/* Cache miss on partial write. Read existing values
			 * surrounding the target [pos, pos+sz] range, which
			 * the caller expects to remain unchanged. */
			if (!cache_read_async(sector, cached, WAIT_FOR_DATA)) {
				goto done;
			}
			ASSERT(cached->state == CACHE_READ_AWAIT_FIRST_USE);
		}
		break;
	case CACHE_CLEAN:
	case CACHE_DIRTY:
	case CACHE_READ_AWAIT_FIRST_USE:
		/* Cache hit. Update existing value. */
		break;
	case CACHE_READ_QUEUED:
		/* cache_find() should never return entries in this state. */
		NOT_REACHED();
		break;
	}

	cached->state = CACHE_DIRTY;
	cached->sector = sector;
	assert_sector_pos_sz_in_range(pos, sz);
	memcpy(cached->data + pos, buffer, sz);
	cached->accessed_at = timer_ticks();

done:
	lock_release(&fs_cache.lock);
	return true;
}
