#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"
#include "threads/thread.h"
#include "vm/page.h"

void frame_init(void);
void *frame_get_page(void *upage, enum palloc_flags flags, enum page_rw rw);
void frame_pin(tid_t tid, void *uaddr, size_t sz);
void frame_unpin(tid_t tid, void *uaddr, size_t sz);
void frame_clear(tid_t tid);

#endif
