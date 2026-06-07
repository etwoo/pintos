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
#include "vm/page.h"

#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>

#define IO_SUCCESS 0 /* Successful IO (when not returning fd or size). */
#define IO_FAIL -1   /* Conceptually distinct from FD_INVALID. */

#define MIN(x, y) ((x) < (y) ? (x) : (y))

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

/* If check succeeds, map uaddr to kaddr and return. */
static void *
check_span_is_user_vaddr(struct intr_frame *f, const void *uaddr, unsigned sz)
{
	const void *uaddr_end = uaddr + sz;
	if (!is_user_vaddr(uaddr) || !is_user_vaddr(uaddr_end)) {
		thread_exit_invalid_pointer_argument(f);
	}

	struct thread *t = thread_current();

	void *begin = pg_round_down(uaddr);
	void *end = pg_round_up(uaddr + sz) - 1;
	for (void *cursor = begin; cursor < end; cursor += PGSIZE) {
		void *kaddr = pagedir_get_page(t->pagedir, cursor);
		if (kaddr == NULL) {
			thread_exit_invalid_pointer_argument(f);
		}
	}

	void *kaddr = pagedir_get_page(t->pagedir, uaddr);
	ASSERT(kaddr != NULL); /* Guaranteed by preceding loop. */
	return kaddr;
}

static void *
syscall_arg_peek(struct intr_frame *f,
                 int *stack,
                 unsigned *got_sz,
                 void **got_uaddr)
{
	void *uaddr = (void *)(*stack); /* uaddr parameter on top of stack */
	if (got_uaddr != NULL) {
		*got_uaddr = uaddr;
	}

	if (got_sz != NULL) {
		/* Check span using size on stack (second arg). */
		*got_sz = *(stack + 1);
		return check_span_is_user_vaddr(f, uaddr, *got_sz);
	}

	/* Probe start of span (string length not yet known). */
	void *kaddr = check_span_is_user_vaddr(f, uaddr, 1);

	void *kaddr_pos = kaddr;
	void *uaddr_pos = uaddr;
	while (true) {
		const size_t span_limit = pg_round_up(uaddr_pos) - uaddr_pos;
		ASSERT(span_limit < PGSIZE);

		void *end = memchr(kaddr_pos, '\0', span_limit);
		if (end != NULL) {
			/* Check last segment of overall span. */
			check_span_is_user_vaddr(f, uaddr_pos, end - kaddr_pos);
			break;
		}

		/* If next uaddr has physical/kernel mapping, keep searching. */
		uaddr_pos = pg_round_up(uaddr_pos) + 1;
		kaddr_pos = check_span_is_user_vaddr(f, uaddr_pos, 1);
	}

	return kaddr;
}

static void NO_RETURN
syscall_halt(void)
{
	shutdown_power_off();
}

static void NO_RETURN
syscall_exit(struct intr_frame *f, int *stack)
{
	const int status = *stack++;
	f->eax = status;
	thread_exit(status);
}

static void
syscall_exec(struct intr_frame *f, int *stack)
{
	void *filename = syscall_arg_peek(f, stack++, NULL, NULL);
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
	void *filename = syscall_arg_peek(f, stack++, NULL, NULL);
	const unsigned sz = *stack++;

	acquire_io_lock();
	const bool created = filesys_create(filename, sz);
	release_io_lock();

	f->eax = created ? 1 : 0; /* create() returns bool, not integer code */
}

static void
syscall_remove(struct intr_frame *f, int *stack)
{
	void *filename = syscall_arg_peek(f, stack++, NULL, NULL);

	acquire_io_lock();
	const bool removed = filesys_remove(filename);
	release_io_lock();

	f->eax = removed ? 1 : 0; /* remove() returns bool, not integer code */
}

static void
syscall_open(struct intr_frame *f, int *stack)
{
	void *filename = syscall_arg_peek(f, stack++, NULL, NULL);

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
	struct file *file = fd_to_file(fd);

	unsigned sz = 0;
	void *uaddr = NULL;
	(void)syscall_arg_peek(f, stack, &sz, &uaddr);
	stack += 2;

	page_pin(uaddr, sz);
	f->eax = IO_FAIL; /* Assume failure, in case of early exit. */

	struct thread *t = thread_current();
	off_t read = 0;
	void *cursor = uaddr;
	const void *end = uaddr + sz;

	if (fd == STDIN_FILENO) {
		uint8_t *p = uaddr;
		*p = input_getc();
		read = 1;
	}

	// TODO: consolidate syscall_read+syscall_write, avoid loop copy pasta
	while (cursor < end && file != NULL) {
		void *kaddr = pagedir_get_page(t->pagedir, cursor);
		ASSERT(kaddr != NULL);

		void *next = pg_round_down(cursor + PGSIZE);
		const size_t to_read = MIN(next - cursor, end - cursor);
		const off_t pos = cursor - uaddr;

		acquire_io_lock();
		const off_t bytes = file_read_at(file, kaddr, to_read, pos);
		release_io_lock();

		if (bytes < 0) {
			read = bytes;
			break;
		}

		read += bytes;
		cursor = next;
	}

	f->eax = read;
	page_unpin(uaddr, sz);
}

static void
syscall_write(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;
	struct file *file = fd_to_file(fd);

	unsigned sz = 0;
	void *uaddr = NULL;
	(void)syscall_arg_peek(f, stack, &sz, &uaddr);
	stack += 2;

	page_pin(uaddr, sz);
	f->eax = IO_FAIL; /* Assume failure, in case of early exit. */

	struct thread *t = thread_current();
	off_t written = 0;
	void *cursor = uaddr;
	const void *end = uaddr + sz;

	if (fd == STDOUT_FILENO) {
		ASSERT(sz < PGSIZE);
		putbuf(pagedir_get_page(t->pagedir, uaddr), sz);
		written += sz;
	}

	while (cursor < end && file != NULL) {
		void *kaddr = pagedir_get_page(t->pagedir, cursor);
		ASSERT(kaddr != NULL);

		void *next = pg_round_down(cursor + PGSIZE);
		const size_t to_write = MIN(next - cursor, end - cursor);
		const off_t pos = cursor - uaddr;

		acquire_io_lock();
		const off_t bytes = file_write_at(file, kaddr, to_write, pos);
		release_io_lock();

		if (bytes < 0) {
			written = bytes;
			break;
		}

		written += bytes;
		cursor = next;
	}

	f->eax = written;
	page_unpin(uaddr, sz);
}

static void
syscall_seek(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;
	const unsigned sz = *stack++;

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
syscall_mmap(struct intr_frame *f, int *stack)
{
	const int fd = *stack++;
	uintptr_t uaddr = *stack++;
	f->eax = page_mmap(fd, (void *)uaddr).id;
}

static void
syscall_munmap(struct intr_frame *f, int *stack)
{
	const struct page_descriptor pd = {
		.id = *stack++,
	};
	page_munmap(pd);
	f->eax = IO_SUCCESS;
}

static void
syscall_handler(struct intr_frame *f)
{
	int *kaddr = check_span_is_user_vaddr(f, f->esp, sizeof(int));
	const int syscall_number = *kaddr++;

	switch (syscall_number) {
	case SYS_HALT:
		syscall_halt();
		break;
	case SYS_EXIT:
		syscall_exit(f, kaddr);
		break;
	case SYS_EXEC:
		syscall_exec(f, kaddr);
		break;
	case SYS_WAIT:
		syscall_wait(f, kaddr);
		break;
	case SYS_CREATE:
		syscall_create(f, kaddr);
		break;
	case SYS_REMOVE:
		syscall_remove(f, kaddr);
		break;
	case SYS_OPEN:
		syscall_open(f, kaddr);
		break;
	case SYS_FILESIZE:
		syscall_filesize(f, kaddr);
		break;
	case SYS_READ:
		syscall_read(f, kaddr);
		break;
	case SYS_WRITE:
		syscall_write(f, kaddr);
		break;
	case SYS_SEEK:
		syscall_seek(f, kaddr);
		break;
	case SYS_TELL:
		syscall_tell(f, kaddr);
		break;
	case SYS_CLOSE:
		syscall_close(f, kaddr);
		break;
	case SYS_MMAP:
		syscall_mmap(f, kaddr);
		break;
	case SYS_MUNMAP:
		syscall_munmap(f, kaddr);
		break;
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
