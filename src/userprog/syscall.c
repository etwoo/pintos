#include "userprog/syscall.h"

#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>

static void syscall_handler(struct intr_frame *);

void
syscall_init(void)
{
	intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void NO_RETURN
thread_exit_invalid_pointer_argument(struct intr_frame *f)
{
	f->eax = EINVAL;
	thread_exit();
	NOT_REACHED();
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
syscall_exec(struct intr_frame *f, long *stack)
{
	(void)f;       // TODO rm
	(void)stack;   // TODO rm
	ASSERT(false); // TODO exec()
}

static void
syscall_wait(struct intr_frame *f, long *stack)
{
	(void)f;       // TODO rm
	(void)stack;   // TODO rm
	ASSERT(false); // TODO wait()
}

static void
syscall_create(struct intr_frame *f, long *stack)
{
	uint32_t *pagedir = thread_current()->pagedir;

	const void *filename_uaddr = (void *)(*stack++);
	if (!is_user_vaddr(filename_uaddr)) {
		thread_exit_invalid_pointer_argument(f);
	}

	void *filename_paddr = pagedir_get_page(pagedir, filename_uaddr);
	if (filename_paddr == NULL) {
		thread_exit_invalid_pointer_argument(f);
	}

	const size_t span = pg_round_up(filename_uaddr) - filename_uaddr;
	ASSERT(span < PGSIZE);
	if (NULL == memchr(filename_paddr, '\0', span)) {
		/* String parameter lacks null terminator. */
		thread_exit_invalid_pointer_argument(f);
	}

	const unsigned sz = *stack++;
	const bool created = filesys_create(filename_paddr, sz);
	f->eax = created ? 1 : 0; /* create() returns bool, not integer code */
}

static void
syscall_remove(struct intr_frame *f, long *stack)
{
	// TODO: use filesys_remove() wrapper?
}

static void
syscall_open(struct intr_frame *f, long *stack)
{
	// TODO: use filesys_open() wrapper?
	// TODO: store int fd -> struct file * mapping in thread_current()?
	// TODO: skip STDIN_FILENO, STDOUT_FILENO (no stderr in pintos)
}

static void
syscall_filesize(struct intr_frame *f, long *stack)
{
	// TODO: map fd -> struct file *, then call file_length()
}

static void
syscall_read(struct intr_frame *f, long *stack)
{
	// TODO: map fd -> struct file *, then call file_read()
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
	/* No buffered I/O, hence no need to copy to a bounce buffer. */

	f->eax = 0; // TODO: set meaningful return value
}

static void
syscall_seek(struct intr_frame *f, long *stack)
{
	// TODO: map fd -> struct file *, then call file_seek()
}

static void
syscall_tell(struct intr_frame *f, long *stack)
{
	// TODO: map fd -> struct file *, then call file_tell()
}

static void
syscall_close(struct intr_frame *f, long *stack)
{
	// TODO: map fd -> struct file *, then call file_close()
	// TODO: clear fd -> struct file * mapping after file_close deallocates
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
		syscall_exec(f, upage);
		break;
	case SYS_WAIT:
		syscall_wait(f, upage);
		break;
	case SYS_CREATE:
		syscall_create(f, upage);
		break;
	case SYS_REMOVE:
		syscall_remove(f, upage);
		break;
	case SYS_OPEN:
		syscall_open(f, upage);
		break;
	case SYS_FILESIZE:
		syscall_filesize(f, upage);
		break;
	case SYS_READ:
		syscall_read(f, upage);
		break;
	case SYS_WRITE:
		syscall_write(f, upage);
		break;
	case SYS_SEEK:
		syscall_seek(f, upage);
		break;
	case SYS_TELL:
		syscall_tell(f, upage);
		break;
	case SYS_CLOSE:
		syscall_close(f, upage);
		break;
	case SYS_MMAP:
	case SYS_MUNMAP:
	case SYS_CHDIR:
	case SYS_MKDIR:
	case SYS_READDIR:
	case SYS_ISDIR:
	case SYS_INUMBER:
		ASSERT(false); // TODO: error on valid+unimplemented syscalls
		break;
	default:
		ASSERT(false); // TODO: ENOSYS on invalid syscall number
		break;
	}
}
