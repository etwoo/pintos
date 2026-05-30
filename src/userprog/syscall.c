#include "userprog/syscall.h"

#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/fd.h"
#include "userprog/io.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>

#define IO_SUCCESS 0 /* Successful IO (when not returning fd or size). */
#define IO_FAIL -1   /* Conceptually distinct from FD_INVALID. */

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
	thread_exit(EXIT_EXCEPTION);
}

enum peek_mode {
	PEEK_CSTRING,
	PEEK_BUFFER,
};

static void *
syscall_arg_peek(struct intr_frame *f, int *stack, enum peek_mode m)
{
	uint32_t *pagedir = thread_current()->pagedir;

	const void *uaddr = (void *)(*stack);
	if (!is_user_vaddr(uaddr)) {
		thread_exit_invalid_pointer_argument(f);
	}

	void *kaddr = pagedir_get_page(pagedir, uaddr);
	if (kaddr == NULL) {
		thread_exit_invalid_pointer_argument(f);
	}

	const size_t span = pg_round_up(uaddr) - uaddr;
	ASSERT(span < PGSIZE);
	if (PEEK_CSTRING == m && NULL == memchr(kaddr, '\0', span)) {
		/* String parameter lacks null terminator. */
		thread_exit_invalid_pointer_argument(f);
	}

	return kaddr;
}

static unsigned
syscall_arg_peek_unsigned_nolimit(int *stack)
{
	ASSERT(sizeof(unsigned) == sizeof(*stack));
	const unsigned sz = *stack;
	return sz;
}

static unsigned
syscall_arg_peek_unsigned(int *stack)
{
	unsigned sz = syscall_arg_peek_unsigned_nolimit(stack);

	/* If provided size value looks crazy, clamp to a reasonable range.
	   Syscalls like read() will then perform short reads instead. */
	if (sz > PGSIZE) {
		sz = PGSIZE;
	}

	return sz;
}

static void NO_RETURN
syscall_halt(void)
{
	shutdown_power_off();
}

static void NO_RETURN
syscall_exit(struct intr_frame *f, int *stack)
{
	const int status = *stack;
	f->eax = status;
	thread_exit(status);
}

static void
syscall_exec(struct intr_frame *f, int *stack)
{
	void *filename = syscall_arg_peek(f, stack++, PEEK_CSTRING);
	f->eax = process_execute(filename);
}

static void
syscall_wait(struct intr_frame *f, int *stack)
{
	const int pid = *stack++; /* Assumes sizeof(pid_t) == sizeof(int). */
	f->eax = process_wait(pid);
}

static void
syscall_create(struct intr_frame *f, int *stack)
{
	void *filename = syscall_arg_peek(f, stack++, PEEK_CSTRING);
	const unsigned sz = syscall_arg_peek_unsigned_nolimit(stack++);

	acquire_io_lock();
	const bool created = filesys_create(filename, sz);
	release_io_lock();

	f->eax = created ? 1 : 0; /* create() returns bool, not integer code */
}

static void
syscall_remove(struct intr_frame *f, int *stack)
{
	void *filename = syscall_arg_peek(f, stack++, PEEK_CSTRING);

	acquire_io_lock();
	const bool removed = filesys_remove(filename);
	release_io_lock();

	f->eax = removed ? 1 : 0; /* remove() returns bool, not integer code */
}

static void
syscall_open(struct intr_frame *f, int *stack)
{
	void *filename = syscall_arg_peek(f, stack++, PEEK_CSTRING);

	acquire_io_lock();
	struct file *fh = filesys_open(filename);
	release_io_lock();

	if (fh == NULL) {
		f->eax = FD_INVALID;
	} else {
		f->eax = fd_register(fh);
	}
}

static void
syscall_filesize(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;

	struct file *file = fd_to_file(fd);
	if (file == NULL) {
		f->eax = FD_INVALID;
	} else {
		acquire_io_lock();
		f->eax = file_length(file);
		release_io_lock();
	}
}

static void
syscall_read(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;
	void *buffer = syscall_arg_peek(f, stack++, PEEK_BUFFER);
	const unsigned sz = syscall_arg_peek_unsigned(stack++);

	if (fd == STDIN_FILENO) {
		if (sz == 0) {
			f->eax = IO_FAIL;
		} else {
			uint8_t *p = buffer;
			*p = input_getc();
			f->eax = 1;
		}
		return;
	}

	struct file *file = fd_to_file(fd);
	if (file == NULL) {
		f->eax = IO_FAIL;
	} else {
		acquire_io_lock();
		f->eax = file_read(file, buffer, sz);
		release_io_lock();
	}
}

static void
syscall_write(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;
	void *buffer = syscall_arg_peek(f, stack++, PEEK_BUFFER);
	const unsigned sz = syscall_arg_peek_unsigned(stack++);

	if (fd == STDOUT_FILENO) {
		// TODO: if sz>512, call putbuf in chunks (console lock perf)
		putbuf(buffer, sz);
		f->eax = sz;
		return;
	}

	struct file *file = fd_to_file(fd);
	if (file == NULL) {
		f->eax = IO_FAIL;
	} else {
		acquire_io_lock();
		f->eax = file_write(file, buffer, sz);
		release_io_lock();
	}
}

static void
syscall_seek(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;
	const unsigned sz = syscall_arg_peek_unsigned(stack++);

	struct file *file = fd_to_file(fd);
	if (file == NULL) {
		f->eax = IO_FAIL;
	} else {
		acquire_io_lock();
		file_seek(file, sz);
		release_io_lock();
		f->eax = IO_SUCCESS;
	}
}

static void
syscall_tell(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;

	struct file *file = fd_to_file(fd);
	if (file == NULL) {
		f->eax = IO_FAIL;
	} else {
		acquire_io_lock();
		f->eax = file_tell(file);
		release_io_lock();
	}
}

static void
syscall_close(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;

	struct file *file = fd_to_file(fd);
	if (file == NULL) {
		f->eax = IO_FAIL;
	} else {
		fd_unregister(fd);
		acquire_io_lock();
		file_close(file);
		release_io_lock();
		f->eax = IO_SUCCESS;
	}
}

static void
syscall_handler(struct intr_frame *f)
{
	const void *uaddr = f->esp;
	if (!is_user_vaddr(uaddr)) {
		thread_exit_invalid_pointer_argument(f);
	}

	int *upage = pagedir_get_page(thread_current()->pagedir, uaddr);
	if (upage == NULL) {
		thread_exit_invalid_pointer_argument(f);
	}

	const int syscall_number = *upage++;
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
	default:
		f->eax = ENOSYS;
		break;
	}
}
