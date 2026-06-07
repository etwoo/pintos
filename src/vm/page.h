#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "filesys/off_t.h"
#include "threads/palloc.h"
#include "threads/thread.h"

enum page_rw {
	PAGE_READONLY,
	PAGE_WRITABLE,
};

void page_init(void);
void page_destroy(void);

bool page_map(int fd, off_t pos, void *upage, enum page_rw rw);
bool page_map_zero(void *upage, enum page_rw rw);
void *page_create(enum palloc_flags extra_flags, void *upage, enum page_rw rw);

#define PAGE_DESCRIPTOR_ERROR -1
struct page_descriptor {
	int id;
};

struct page_descriptor page_mmap(int fd, void *upage);
void page_munmap(struct page_descriptor pd);

void page_pin(void *uaddr, size_t sz);
void page_unpin(void *uaddr, size_t sz);

struct intr_frame;
bool page_fault_on(struct intr_frame *f, void *uaddr);

void page_evict(tid_t owner, void *upage);

#endif
