#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "filesys/off_t.h"
#include "threads/palloc.h"

enum page_rw {
	PAGE_READONLY,
	PAGE_WRITABLE,
};

void page_init(void);
bool page_map(int fd, off_t pos, void *upage, enum page_rw rw);
bool page_map_zero(void *upage, enum page_rw rw);
void *page_create(enum palloc_flags extra_flags, void *upage, enum page_rw rw);

bool page_fault_on(void *uaddr);

#endif
