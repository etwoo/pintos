#include "userprog/syscall.h"

#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
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

static void NO_RETURN
thread_exit_malloc_fail(struct intr_frame *f)
{
	f->eax = ENOMEM;
	thread_exit(EXIT_EXCEPTION);
}

/* See __executable_start in ./src/lib/user/user.lds */
static const void *VADDR_CODE_SEGMENT = (void *)0x08048000;

/* If check succeeds, map uaddr to kaddr and return. */
static void *
check_span_is_user_vaddr(struct intr_frame *f, const void *uaddr, unsigned sz)
{
	const void *uaddr_end = uaddr + sz;
	if (!is_user_vaddr(uaddr) || !is_user_vaddr(uaddr_end)) {
		thread_exit_invalid_pointer_argument(f);
	}
	if (pg_round_down(uaddr) == VADDR_CODE_SEGMENT ||
	    pg_round_down(uaddr_end) == VADDR_CODE_SEGMENT) {
		thread_exit_invalid_pointer_argument(f);
	}

	struct thread *t = thread_current();

	void *begin = pg_round_down(uaddr);
	void *end = pg_round_up(uaddr + sz) - 1;
	for (void *cursor = begin; cursor < end; cursor += PGSIZE) {
		void *kaddr = pagedir_get_page(t->pagedir, cursor);
		if (kaddr == NULL) {
#ifdef VM
			if (!page_fault_on(f, cursor))
#endif
			{
				thread_exit_invalid_pointer_argument(f);
			}
		}
	}

	void *kaddr = pagedir_get_page(t->pagedir, uaddr);
	ASSERT(kaddr != NULL); /* Guaranteed by preceding loop. */
	return kaddr;
}

static void
syscall_arg_peek(struct intr_frame *f,
                 int *stack,
                 void **got_buffer_uaddr,
                 unsigned *got_buffer_sz,
                 char **got_cstring)
{
	void *uaddr = (void *)(*stack); /* uaddr parameter on top of stack */
	if (got_buffer_uaddr != NULL) {
		*got_buffer_uaddr = uaddr;
	}

	if (got_buffer_sz != NULL) {
		/* Check span using size on stack (second arg). */
		*got_buffer_sz = *(stack + 1);
		check_span_is_user_vaddr(f, uaddr, *got_buffer_sz);
		return;
	}

	/* Probe start of span (string length not yet known). */
	void *kaddr = check_span_is_user_vaddr(f, uaddr, 1);

	const size_t span_limit = pg_round_up(uaddr) - uaddr;
	ASSERT(span_limit > 0 && span_limit <= PGSIZE);

	struct {
		char *str;
		size_t len;
	} spans[2] = {0};

	void *end = memchr(kaddr, '\0', span_limit);
	if (end != NULL) {
		spans[0].str = kaddr;
		spans[0].len = end - kaddr;
	} else {
		/* The remainder of this page lacks this string's null
		 * terminator. Search the next page (in uaddr space). */
		spans[0].str = kaddr;
		spans[0].len = span_limit;

		kaddr = check_span_is_user_vaddr(f, uaddr + span_limit, 1);

		end = memchr(kaddr, '\0', PGSIZE);
		if (end == NULL) {
			/* Next page lacks null terminator as well. */
			thread_exit_invalid_pointer_argument(f);
		}

		spans[1].str = kaddr;
		spans[1].len = end - kaddr;
	}

	const size_t len = spans[0].len + spans[1].len;
	char *bounce = malloc(len + 1);
	if (bounce == NULL) {
		thread_exit_malloc_fail(f);
	}

	memcpy(bounce, spans[0].str, spans[0].len);
	memcpy(bounce + spans[0].len, spans[1].str, spans[1].len);
	bounce[len] = '\0';

	ASSERT(got_cstring != NULL);
	*got_cstring = bounce;
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
	char *filename = NULL;
	syscall_arg_peek(f, stack++, NULL, NULL, &filename);

	f->eax = process_execute(filename);
	free(filename);
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
	char *filename = NULL;
	syscall_arg_peek(f, stack++, NULL, NULL, &filename);
	const unsigned sz = *stack++;

	acquire_io_lock();
	const bool created = filesys_create(filename, sz);
	release_io_lock();

	f->eax = created ? 1 : 0; /* create() returns bool, not integer code */
	free(filename);
}

static void
syscall_remove(struct intr_frame *f, int *stack)
{
	char *filename = NULL;
	syscall_arg_peek(f, stack++, NULL, NULL, &filename);

	acquire_io_lock();
	const bool removed = filesys_remove(filename);
	release_io_lock();

	f->eax = removed ? 1 : 0; /* remove() returns bool, not integer code */
	free(filename);
}

static void
syscall_open(struct intr_frame *f, int *stack)
{
	char *filename = NULL;
	syscall_arg_peek(f, stack++, NULL, NULL, &filename);

	acquire_io_lock();
	struct file *fh = filesys_open(filename);
	release_io_lock();

	if (fh == NULL) {
		f->eax = FD_INVALID;
	} else {
		f->eax = fd_register(fh);
	}
	free(filename);
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
syscall_io(int syscall_number, struct intr_frame *f, int *stack)
{
	const int fd = *stack++;
	struct file *file = fd_to_file(fd);

	void *uaddr = NULL;
	unsigned sz = 0;
	syscall_arg_peek(f, stack, &uaddr, &sz, NULL);
	stack += 2;

	struct thread *t = thread_current();
	off_t total_bytes = 0;
#ifdef VM
	page_pin(uaddr, sz);
#endif
	switch (syscall_number) {
	case SYS_READ:
		if (fd == STDIN_FILENO) {
			uint8_t *p = uaddr;
			*p = input_getc();
			total_bytes = 1;
		}
		break;
	case SYS_WRITE:
		if (fd == STDOUT_FILENO) {
			ASSERT(sz < PGSIZE);
			putbuf(pagedir_get_page(t->pagedir, uaddr), sz);
			total_bytes += sz;
		}
		break;
	default:
		NOT_REACHED();
		break;
	}

	while (total_bytes < (off_t)sz && file != NULL) {
		void *cursor = uaddr + total_bytes;
		void *kaddr = pagedir_get_page(t->pagedir, cursor);
		ASSERT(kaddr != NULL);

		const size_t to_next = pg_round_down(cursor + PGSIZE) - cursor;
		const size_t segment = MIN(to_next, sz - total_bytes);
		off_t bytes = 0;

		acquire_io_lock();
		switch (syscall_number) {
		case SYS_READ:
			bytes = file_read(file, kaddr, segment);
			break;
		case SYS_WRITE:
			bytes = file_write(file, kaddr, segment);
			break;
		default:
			NOT_REACHED();
			break;
		}
		release_io_lock();

		if (bytes <= 0) {
			if (bytes < 0) {
				total_bytes = bytes;
			} /* else: reached EOF. */
			break;
		}

		total_bytes += bytes;
	}
#ifdef VM
	page_unpin(uaddr, sz);
#endif
	f->eax = total_bytes;
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
#ifdef VM
	const int fd = *stack++;
	uintptr_t uaddr = *stack++;
	f->eax = page_mmap(fd, (void *)uaddr).id;
#else
	(void)stack; /* Unused. */
	f->eax = ENOSYS;
#endif
}

static void
syscall_munmap(struct intr_frame *f, int *stack)
{
#ifdef VM
	const struct page_descriptor pd = {
		.id = *stack++,
	};
	page_munmap(pd);
	f->eax = IO_SUCCESS;
#else
	(void)stack; /* Unused. */
	f->eax = ENOSYS;
#endif
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
	case SYS_WRITE:
		syscall_io(syscall_number, f, kaddr);
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
