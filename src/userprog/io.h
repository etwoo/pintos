#ifndef USERPROG_IO_H
#define USERPROG_IO_H

void io_init(void);
void acquire_io_lock(void);
void release_io_lock(void);

#endif
