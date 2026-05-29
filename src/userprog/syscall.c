#include "userprog/syscall.h"

#include "devices/shutdown.h"
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
syscall_halt(void)
{
	shutdown_power_off();
}

static void
syscall_exit(struct intr_frame *f, long *stack)
{
	const long status = *stack;
	f->eax = status;
	thread_exit();
}

static void
syscall_write(struct intr_frame *f, long *stack)
{
	const long fd = *stack;
	// printf("got fd %ld\n", fd);
	ASSERT(fd == STDOUT_FILENO); // TODO validate fd

	++stack;

	const uintptr_t buffer_uaddr = *stack;
	// printf("got buffer_uaddr %p\n", (void *)buffer_uaddr);

	// TODO: validate buffer_uaddr before calling pagedir_get_page
	void *buffer_paddr = pagedir_get_page(thread_current()->pagedir,
	                                      (void *)buffer_uaddr);
	// printf("got buffer_paddr %p\n", (void *)buffer_paddr);

	++stack;

	const long sz = *stack; // TODO: clamp buffer size?
	// printf("got size %ld\n", sz);

	// printf("best-effort buffer_paddr data:\n");
	// printf("\n");
	// hex_dump(buffer_uaddr, buffer_paddr, sz, true); // TODO rm hex_dump()
	// hex_dump(0, buffer_paddr, sz, true); // TODO rm hex_dump()

	// TODO: handle write() more generally
	// TODO: if sz>512, call putbuf() on chunks, avoid holding console_lock
	// for too long at once
	putbuf(buffer_paddr, sz);

	f->eax = 0; // TODO: set meaningful return value
}

static void
syscall_handler(struct intr_frame *f)
{
	// TODO: validate esp before calling pagedir_get_page
	long *upage = pagedir_get_page(thread_current()->pagedir, f->esp);
	ASSERT(upage != NULL); // TODO: error cleanly
	// printf("got upage %p\n", upage);

	const long syscall_number = *upage;
	++upage;

	switch (syscall_number) {
	case SYS_HALT:
		syscall_halt();
		break;
	case SYS_EXIT:
		syscall_exit(f, upage);
		break;
	case SYS_EXEC:
		break;
	case SYS_WAIT:
		break;
	case SYS_CREATE:
		break;
	case SYS_REMOVE:
		break;
	case SYS_OPEN:
		break;
	case SYS_FILESIZE:
		break;
	case SYS_READ:
		break;
	case SYS_WRITE:
		syscall_write(f, upage);
		break;
	case SYS_SEEK:
		break;
	case SYS_TELL:
		break;
	case SYS_CLOSE:
		break;
	case SYS_MMAP:
	case SYS_MUNMAP:
		// TODO: return error on unimplemented project 3 syscalls
		break;
	case SYS_CHDIR:
	case SYS_MKDIR:
	case SYS_READDIR:
	case SYS_ISDIR:
	case SYS_INUMBER:
		// TODO: return error on unimplemented project 4 syscalls
		break;
	default:
		ASSERT(false); // TODO: return error on invalid syscall number
		break;
	}
}
