#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>

void page_init(struct hash *page_table);
void *page_create_eager(void *upage, bool writable);
void *page_create_eager_zero(void *upage, bool writable);

bool page_fault_on(void *uaddr);

#endif
