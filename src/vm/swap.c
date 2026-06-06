#include "vm/swap.h"

#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#include <bitmap.h>

#define SWAP_SLOT_INVALID SIZE_MAX

struct swap_partition {
	struct lock lock;
	struct block *block_device;
	struct bitmap *used_map; /* Bitmap of available swap slots. */
};

static struct swap_partition swap;

void
swap_init(void)
{
	lock_init(&swap.lock);

	ASSERT(swap.block_device == NULL);
	swap.block_device = block_get_role(BLOCK_SWAP);
	ASSERT(swap.block_device != NULL);

	const block_sector_t sector_count = block_size(swap.block_device);
	const size_t total_slots = sector_count * BLOCK_SECTOR_SIZE / PGSIZE;
	ASSERT(PGSIZE % BLOCK_SECTOR_SIZE == 0);

	swap.used_map = bitmap_create(total_slots);
	ASSERT(swap.used_map != NULL);
	/* <used_map> lives forever. We never call bitmap_destroy(). */
}

bool
swap_slot_is_valid(struct swap_slot slot)
{
	return slot.slot != SWAP_SLOT_INVALID;
}

void
swap_slot_unset(struct swap_slot *slot)
{
	slot->slot = SWAP_SLOT_INVALID;
}

struct swap_slot
swap_save(void *kpage)
{
	lock_acquire(&swap.lock);
	const size_t chosen = bitmap_scan_and_flip(swap.used_map, 0, 1, false);
	lock_release(&swap.lock);

	if (chosen == BITMAP_ERROR) {
		return (struct swap_slot){
			.slot = SWAP_SLOT_INVALID,
		};
	}

	const block_sector_t start = chosen * (PGSIZE / BLOCK_SECTOR_SIZE);
	ASSERT(PGSIZE % BLOCK_SECTOR_SIZE == 0);

	for (size_t i = 0; i < PGSIZE; i += BLOCK_SECTOR_SIZE) {
		block_write(swap.block_device, start + i, kpage + i);
	}

	return (struct swap_slot){
		.slot = chosen,
	};
}

void
swap_load(struct swap_slot s, void *kpage)
{
	ASSERT(swap_slot_is_valid(s));

	const block_sector_t start = s.slot * (PGSIZE / BLOCK_SECTOR_SIZE);
	ASSERT(PGSIZE % BLOCK_SECTOR_SIZE == 0);

	for (size_t i = 0; i < PGSIZE; i += BLOCK_SECTOR_SIZE) {
		block_read(swap.block_device, start + i, kpage + i);
	}

	lock_acquire(&swap.lock);
	ASSERT(bitmap_test(swap.used_map, s.slot));
	bitmap_reset(swap.used_map, s.slot);
	lock_release(&swap.lock);
}
