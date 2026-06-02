#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#define ENOMEM 12 /* Cannot allocate memory */
#define EINVAL 22 /* Invalid argument */
#define ENOSYS 78 /* Function not implemented */

void syscall_init(void);

#endif /* userprog/syscall.h */
