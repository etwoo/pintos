#ifndef VM_FRAME_H
#define VM_FRAME_H

void frame_init(void);
void *frame_create(void *upage, bool writable);
void *frame_create_zero(void *upage, bool writable);

#endif
