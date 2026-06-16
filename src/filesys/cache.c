#include "filesys/cache.h"

#include "devices/timer.h"
#include "filesys/filesys.h"
#include "filesys/inode_disk.h" /* for INODE_SECTOR_UNSET */
#include "threads/malloc.h"
#include "threads/thread.h"

#include <array.h>
#include <debug.h>
// #include <stdio.h> // TODO rm, printf
#include <string.h>

#define printf(...) // TODO rm

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

		block_sector_t sector = r->block->sector;
		if (r->op == REQUEST_DRAIN_AND_TEARDOWN) {
			ASSERT(r->block->sector == CACHE_SECTOR_UNSET);
			ASSERT(r->sector_extra != CACHE_SECTOR_UNSET);
			sector = r->sector_extra;
		}

		lock_release(&fs_cache.lock);
		switch (r->op) {
		case REQUEST_READ:
			// printf("block_read() %p %d\n", r->block, r->block->sector);
			block_read(fs_device, sector, r->block->data);
			break;
		case REQUEST_WRITE:
		case REQUEST_DRAIN_AND_TEARDOWN:
			// printf("block_write() %p %d\n", r->block, r->block->sector);
			block_write(fs_device, sector, r->block->data);
			break;
		}
		lock_acquire(&fs_cache.lock);

		if (r->op == REQUEST_DRAIN_AND_TEARDOWN) {
			ASSERT(r->block->sector == CACHE_SECTOR_UNSET);
		} else {
			ASSERT(r->block->sector == sector);
		}
		r->block->state = r->block->io_async.ready_state;
		if (r->block->state == CACHE_IO_AWAIT_FIRST_USE) {
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

enum read_wait_mode {
	WAIT_FOR_READ,
	WAIT_FOR_WRITE,
	WAIT_AND_CLAIM,
	RETURN_AFTER_ENQUEUE,
};

static bool
cache_io_async(enum cache_request_op op,
               struct cache_block *cache,
               enum read_wait_mode mode,
	       block_sector_t sector_extra)
{
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	ASSERT(cache->sector == CACHE_SECTOR_UNSET || cache->sector < block_size(fs_device));

	struct cache_request *r = malloc(sizeof(*r));
	if (r == NULL) {
		return false;
	}

	cache->state = CACHE_IO_QUEUED;
	if (sector_extra != CACHE_SECTOR_UNSET) {
		ASSERT(cache->sector == CACHE_SECTOR_UNSET);
	} else {
		ASSERT(cache->sector != CACHE_SECTOR_UNSET);
	}

	switch (mode) {
	case WAIT_FOR_READ:
		/* Caller waits synchronously and reads at least once. */
		cache->io_async.ready_state = CACHE_IO_AWAIT_FIRST_USE;
		break;
	case WAIT_FOR_WRITE:
		/* Caller waits synchronously but does not read. */
		cache->io_async.ready_state = CACHE_CLEAN;
		break;
	case WAIT_AND_CLAIM:
		cache->io_async.ready_state = CACHE_IO_AWAIT_FIRST_USE;
		break;
	case RETURN_AFTER_ENQUEUE:
		/* Caller returns before operation completes. */
		cache->io_async.ready_state = CACHE_CLEAN;
		break;
	}

	r->op = op;
	r->sector_extra = sector_extra;
	r->block = cache;
	list_push_back(&fs_cache.requests, &r->elem);

	/* cache_io_thread() takes ownership of cache_request memory. */
	r = NULL;

	cond_signal(&fs_cache.requests_pending, &fs_cache.lock);

	const block_sector_t io_before_wait = cache->sector;
	switch (mode) {
	case WAIT_FOR_READ:
	case WAIT_FOR_WRITE:
	case WAIT_AND_CLAIM:
		while (cache->state == CACHE_IO_QUEUED) {
			cond_wait(&cache->io_async.ready, &fs_cache.lock);
		}
		if (mode == WAIT_FOR_READ || mode == WAIT_AND_CLAIM) {
			if (op != REQUEST_DRAIN_AND_TEARDOWN &&
			    io_before_wait != cache->sector) {
				printf("%s() WTF3 %d != %d\n", __func__, io_before_wait, cache->sector);
			}
			ASSERT(op == REQUEST_DRAIN_AND_TEARDOWN || io_before_wait == cache->sector);
		}
		break;
	case RETURN_AFTER_ENQUEUE:
		break;
	}

	return true;
}

static bool
cache_read_async(block_sector_t sector,
                 struct cache_block *to_fill,
                 enum read_wait_mode mode)
{
	// printf("%s() %p %d start\n", __func__, to_fill, sector);
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	to_fill->sector = sector;
	return cache_io_async(REQUEST_READ, to_fill, mode, CACHE_SECTOR_UNSET);
}

static bool
cache_flush_async_and_claim_atomically(struct cache_block *to_flush)
{
	// printf("%s() %p %d start\n", __func__, to_flush, to_flush->sector);
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	const block_sector_t sector_extra = to_flush->sector;
	to_flush->sector = CACHE_SECTOR_UNSET; /* Deny new IO for this block. */
	return cache_io_async(REQUEST_DRAIN_AND_TEARDOWN, to_flush, WAIT_AND_CLAIM, sector_extra);
}

static bool
cache_flush_async(struct cache_block *to_flush)
{
	// printf("%s() %p %d start\n", __func__, to_flush, to_flush->sector);
	ASSERT(lock_held_by_current_thread(&fs_cache.lock));
	return cache_io_async(REQUEST_WRITE, to_flush, WAIT_FOR_WRITE, CACHE_SECTOR_UNSET);
}

static void
cache_optional_readahead(block_sector_t hint)
{
	// TODO: reenable readahead, after checking if race is simpler w/o it
	return;

	// printf("%s() %d start\n", __func__, hint);
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
				// TODO: refactor
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
	cache_read_async(hint, oldest, RETURN_AFTER_ENQUEUE);
	/* Do not wait for request to complete. Also ignore failure. */
}

static bool
cache_find(block_sector_t sector, struct cache_block **cached)
{
	// printf("%s() %d start\n", __func__, sector);
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
				b->io_async.ready_state = CACHE_IO_AWAIT_FIRST_USE;
				b->io_async.awaiting++;
				printf("%s() wait %p %d/%d on CACHE_IO_QUEUED awaiting %d\n",
				       __func__,
				       b,
				       sector, b->sector, b->io_async.awaiting);
				while (b->state == CACHE_IO_QUEUED) {
					cond_wait(&b->io_async.ready,
					          &fs_cache.lock);
				}
				ASSERT(b->io_async.awaiting > 0);
				if (--b->io_async.awaiting == 0) {
					b->state = b->io_async.ready_state;
					b->io_async.ready_state = CACHE_UNUSED;
					cond_signal(&b->io_async.ready, &fs_cache.lock);
				}
				while (b->state == CACHE_IO_QUEUED || b->io_async.awaiting > 0) {
					printf("%s() wait(cache-hit) %p %d/%d to drain awaiting %d\n",
					       __func__,
					       b,
					       sector, b->sector, b->io_async.awaiting);
					cond_wait(&b->io_async.ready, &fs_cache.lock);
				}
				// TODO? b->state = b->io_async.ready_state;
				if (sector != b->sector && b->sector != CACHE_SECTOR_UNSET) {
					printf("%s() WTF1 %p %d != %d, awaiting=%d\n",
					       __func__,
					       b,
					       sector,
					       b->sector, b->io_async.awaiting);
				}
				ASSERT(b->sector == sector || b->sector == CACHE_SECTOR_UNSET); // see TODO
				*cached = b;
				return true;
			case CACHE_IO_AWAIT_FIRST_USE:
			case CACHE_CLEAN:
			case CACHE_DIRTY:
				/* Cache hit. Return in-memory data. */
				*cached = b;
				// printf("%s() request=%d %p actual=%d return cache hit\n", __func__, sector, b, b->sector);
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
				// TODO: refactor
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
	const block_sector_t oldest_before_flush = oldest->sector;

	ASSERT(oldest->state != CACHE_IO_AWAIT_FIRST_USE && oldest->state != CACHE_IO_QUEUED);

	if (oldest->state == CACHE_DIRTY) {
		ASSERT(oldest->state != CACHE_IO_AWAIT_FIRST_USE && oldest->state != CACHE_IO_QUEUED);
		// if (oldest->io_async.awaiting > 0) {
		// 	printf("Flushing dirty %p %d with nonzero awaiting %d to reuse for %d\n", oldest, oldest->sector, oldest->io_async.awaiting, sector);
		// }
		if (!cache_flush_async_and_claim_atomically(oldest)) {
			ASSERT(oldest->state != CACHE_IO_AWAIT_FIRST_USE);
			return false;
		}
		ASSERT(oldest->state != CACHE_IO_QUEUED && oldest->state != CACHE_DIRTY);
		// ASSERT(oldest_before_flush == oldest->sector); // REQUEST_DRAIN_AND_TEARDOWN changes
		// ASSERT(oldest->state == CACHE_IO_AWAIT_FIRST_USE);
		// if (oldest->io_async.awaiting > 1) {
		// 	printf("After flushing dirty %p %d will decrement awaiting %d to reuse for %d\n", oldest, oldest->sector, oldest->io_async.awaiting, sector);
		// }
		ASSERT(oldest->io_async.awaiting > 0);
		if (--oldest->io_async.awaiting == 0) {
			oldest->state = oldest->io_async.ready_state;
			oldest->io_async.ready_state = CACHE_UNUSED;
			cond_signal(&oldest->io_async.ready, &fs_cache.lock);
		}
		while (oldest->state == CACHE_IO_QUEUED || oldest->io_async.awaiting > 0) {
			printf("%s() wait(evict) %p %d/%d to drain awaiting %d\n",
			       __func__,
			       oldest,
			       oldest_before_flush, oldest->sector, oldest->io_async.awaiting);
			cond_wait(&oldest->io_async.ready, &fs_cache.lock);
		}
		ASSERT(oldest->state != CACHE_IO_AWAIT_FIRST_USE && oldest->state != CACHE_IO_QUEUED && oldest->state != CACHE_DIRTY);
	}
	ASSERT(oldest->state != CACHE_IO_AWAIT_FIRST_USE && oldest->state != CACHE_IO_QUEUED && oldest->state != CACHE_DIRTY);

	/* TODO: looks like problem is:
	 *
	 * 1) caller issues regular read at sector S0
	 * 2) after S0, readahead starts on sector S1
	 * 3) cache_optional_readahead() on S1 calls cache_read_async()
	 * 4) cache_read_async() for readahead marks S1 as CACHE_IO_QUEUED
	 * 5) cache_read_async() for readahead enqueues, waits for IO thread
	 * 6) caller issues regular read at sector S1 (matching readahead)
	 * 7) cache_read_with_readhead() on S1 calls cache_find() and
	 *    encounters block corresponding to in-flight readahead request for
	 *    S1, in state CACHE_IO_QUEUED; calls cond_wait() in response
	 *     a) note: cache_optional_readahead() does _not_ use cache_find()
	 * 8) IO thread wakes and calls block_read() for sector S1, marks cache
	 *    entry CACHE_CLEAN (readahead does not enforce at-least-one-read)
	 * 9) caller issues write at sector S2
	 * 10) cache_write() on S2 calls cache_find()
	 * 11) cache_find() chooses cache block of S1 for eviction, which is in
	 *     CACHE_CLEAN state instead of CACHE_IO_AWAIT_FIRST_USE, due to
	 *     origin as readahead read instead of regular read
	 * 12) cache_find() calls cache_block_reset() and returns
	 * 13) cache_write() hits fastpath for sz == BLOCK_SECTOR_SIZE
	 * 14) cache_write() sets cache block to point to S2, sets CACHE_DIRTY
	 *     a) note: cache_write() can in general (and likely in this case)
	 *        avoid I/0 entirely, performing only in-memory operations on
	 *        buffer cache
	 * 15) regular read wakes up from step 7 in cache_find() in response to
	 *     IO thread cond_broadcast() and change from CACHE_IO_QUEUED
	 *     state, finds block has changed out from underneath to point at
	 *     S2 instead of the expected S1, asserts on this condition
	 *
	 * Earlier hypothesis:
	 *
	 * 1) thread A chooses oldest cache entry C to evict in cache_find()
	 * 2) cache entry C is dirty, so thread A calls cache_flush_async() to
	 *    write to disk
	 * 3) thread A enqueues IO request, yields, and releases lock
	 * 4) thread B makes a read request, calls cache_find(), and gets cache
	 *    hit on C being flushed, with state CACHE_IO_QUEUED
	 * 5) thread B waits on cache entry C to transition out of
	 *    CACHE_IO_QUEUED
	 * 6) IO thread flushes C to disk
	 * 7) thread A wakes and evicts cache entry C from memory, calls
	 *    cache_block_reset(), and starts to reuse it (e.g. calls
	 *    cache_read_async)
	 * 8) thread B wakes on cache entry C leaving state CACHE_IO_QUEUED ->
	 *    CACHE_CLEAN
	 * 9) thread B now finds cache entry C has been changed to refer to a
	 *    different sector, by thread A in step 7
	 */
	if (oldest_before_flush != oldest->sector && oldest->sector != CACHE_SECTOR_UNSET) {
		printf("%s() WTF2 %d != %d\n", __func__, oldest_before_flush, oldest->sector);
	}
	ASSERT(oldest_before_flush == oldest->sector || oldest->sector == CACHE_SECTOR_UNSET);

	// const block_sector_t before_reset = oldest->sector;
	cache_block_reset(oldest);
	*cached = oldest;
	// printf("%s() request=%d %p actual=%d return LRU evict of previous=%d\n", __func__, sector, oldest, oldest->sector, before_reset);
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
	// printf("%s() %d start\n", __func__, sector);
	lock_acquire(&fs_cache.lock);

	struct cache_block *cached = NULL;
	if (!cache_find(sector, &cached)) {
		goto done;
	}
	ASSERT(cached != NULL);

	switch (cached->state) {
	case CACHE_UNUSED:
		// printf("%s() populate cache miss %p %d\n", __func__, cached, sector);
		/* Cache miss. Populate assigned cache entry. */
		if (!cache_read_async(sector, cached, WAIT_FOR_READ)) {
			goto done;
		}
		ASSERT(cached->io_async.awaiting > 0);
		if (--cached->io_async.awaiting == 0) {
			cached->state = CACHE_CLEAN;
			cond_signal(&cached->io_async.ready, &fs_cache.lock);
		}
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

	cache_optional_readahead(hint);

done:
	lock_release(&fs_cache.lock);
	return true;
}

bool
cache_write(block_sector_t sector, int pos, int sz, const void *buffer)
{
	// printf("%s() %d start\n", __func__, sector);
	lock_acquire(&fs_cache.lock);

	struct cache_block *cached = NULL;
	if (!cache_find(sector, &cached)) {
		goto done;
	}
	ASSERT(cached != NULL);

	switch (cached->state) {
	case CACHE_UNUSED:
		if (sz < BLOCK_SECTOR_SIZE) {
			// printf("%s() read before partial write %p %d\n", __func__, cached, sector);
			/* Cache miss on partial write. Read existing values
			 * surrounding the target [pos, pos+sz] range, which
			 * the caller expects to remain unchanged. */
			if (!cache_read_async(sector, cached, WAIT_FOR_READ)) {
				goto done;
			}
			ASSERT(cached->io_async.awaiting > 0);
			if (--cached->io_async.awaiting == 0) {
				cached->state = cached->io_async.ready_state;
				cached->io_async.ready_state = CACHE_UNUSED;
				cond_signal(&cached->io_async.ready, &fs_cache.lock);
			}
			while (cached->state == CACHE_IO_QUEUED ||cached->io_async.awaiting > 0) {
				printf("%s() wait(read-before-write) %p %d/%d to drain awaiting %d\n",
				       __func__,
				       cached,
				       sector, cached->sector, cached->io_async.awaiting);
				cond_wait(&cached->io_async.ready, &fs_cache.lock);
			}
			ASSERT(cached->state != CACHE_IO_AWAIT_FIRST_USE);
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

	cached->state = CACHE_DIRTY;
	cached->sector = sector;
	assert_sector_pos_sz_in_range(pos, sz);
	memcpy(cached->data + pos, buffer, sz);
	cached->accessed_at = timer_ticks();

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
