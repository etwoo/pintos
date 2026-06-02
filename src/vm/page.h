#ifndef PAGE_FRAME_H
#define PAGE_FRAME_H

void page_init(struct hash *page_table);
void *page_create(void *upage, bool writable);
void *page_create_zero(void *upage, bool writable);

bool page_fault_on(void *uaddr);

#endif
