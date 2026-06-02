#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

struct swap_slot {
	size_t slot;
};

void swap_init(void);
bool swap_slot_is_valid(struct swap_slot slot);
void swap_slot_unset(struct swap_slot *slot);
struct swap_slot swap_save(void *kpage);
void swap_load(struct swap_slot s, void *kpage);

#endif
