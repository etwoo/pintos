#include "userprog/syscall.h"

#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"

#include <stdio.h>
#include <syscall-nr.h>

static void syscall_handler(struct intr_frame *);

void
syscall_init(void)
{
	intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler(struct intr_frame *f UNUSED)
{
	uint32_t *pagedir = thread_current()->pagedir;

	// TODO: validate esp before calling pagedir_get_page
	long *upage = pagedir_get_page(pagedir, f->esp);
	ASSERT(upage != NULL); // TODO: error cleanly
	printf("got upage %p\n", upage);

	const long syscall_number = *upage;
	printf("got syscall %ld\n", syscall_number);
	ASSERT(syscall_number == SYS_WRITE); // TODO validate syscall_number

	++upage;

	const long fd = *upage;
	printf("got fd %ld\n", fd);
	ASSERT(fd == STDOUT_FILENO); // TODO validate fd

	++upage;

	const uintptr_t buffer_uaddr = *upage;
	printf("got buffer_uaddr %p\n", (void *)buffer_uaddr);

	// TODO: validate buffer_uaddr before calling pagedir_get_page
	void *buffer_paddr = pagedir_get_page(pagedir, (void *)buffer_uaddr);
	printf("got buffer_paddr %p\n", (void *)buffer_paddr);

	++upage;

	const long sz = *upage; // TODO: clamp buffer size?
	printf("got size %ld\n", sz);

	printf("best-effort buffer_paddr data:\n");
	hex_dump(buffer_uaddr, buffer_paddr, sz, true); // TODO rm hex_dump()

	printf("system call!\n");
	thread_exit();
}
