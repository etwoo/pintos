#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"
#include "vm/page.h"

void frame_init(void);
void *frame_get_page(void *upage, enum palloc_flags flags, enum page_rw rw);
// TODO: frame_clear() on process_exit()? clear ft of entries for dying thread,
// but no need to palloc_free_page() since pagedir_destroy() should handle

#endif
