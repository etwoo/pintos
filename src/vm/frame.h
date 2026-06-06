#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"
#include "vm/page.h"

void frame_init(void);
void *frame_get_page(void *upage, enum palloc_flags flags, enum page_rw rw);

#endif
