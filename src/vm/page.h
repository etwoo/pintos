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

#define PAGE_DESCRIPTOR_ERROR -1
struct page_descriptor {
	int id;
};

struct page_descriptor page_mmap(int fd, void *upage);
void page_munmap(struct page_descriptor pd);

bool page_fault_on(void *uaddr);

#endif
