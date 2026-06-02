#include "userprog/process.h"

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/fd.h"
#include "userprog/gdt.h"
#include "userprog/io.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "vm/page.h"

#include <array.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static thread_func start_process NO_RETURN;
static bool prepare_executable_and_arguments(struct intr_frame *, char *);

struct start_process_args {
	char *file_name;
	struct semaphore child_ready;
	tid_t child_tid;
};

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute(const char *file_name)
{
	struct start_process_args spa = {0};
	sema_init(&spa.child_ready, 0);
	spa.child_tid = TID_ERROR;

	struct thread_wait_code *twc = malloc(sizeof(*twc));
	if (twc == NULL) {
		goto err;
	}

	/* Make a copy of FILE_NAME.
	   Otherwise there's a race between the caller and load(). */
	spa.file_name = palloc_get_page(0);
	if (spa.file_name == NULL) {
		goto err;
	}
	strlcpy(spa.file_name, file_name, PGSIZE);

	char debug_name[sizeof(thread_current()->name)];
	strlcpy(debug_name, spa.file_name, sizeof(debug_name));
	for (size_t i = 0; i < sizeof(debug_name); ++i) {
		if (debug_name[i] == ' ') {
			debug_name[i] = '\0';
			break;
		}
	}

	/* Create a new thread to execute FILE_NAME. */
	const tid_t tentative_tid =
		thread_create(debug_name, PRI_DEFAULT, start_process, &spa);
	if (tentative_tid == TID_ERROR) {
		goto err;
	}

	/* Thread started. Now wait for load() in thread_func. */
	sema_down(&spa.child_ready);

	if (spa.child_tid != TID_ERROR) {
		/* On successful load(), register child_tid with parent. */
		ASSERT(twc != NULL);
		twc->tid = spa.child_tid;
		twc->code = EXIT_UNSET; /* Will update in process_exit(). */
		struct thread *parent = thread_current();
		lock_acquire(&parent->wait.lock);
		list_push_back(&parent->wait.children, &twc->elem);
		lock_release(&parent->wait.lock);
	} else {
		free(twc);
	}

	ASSERT(spa.child_tid == tentative_tid || spa.child_tid == TID_ERROR);
	return spa.child_tid;

err:
	if (twc != NULL) {
		free(twc);
	}
	if (spa.file_name != NULL) {
		/* On fail-fast, retain str ownership. */
		palloc_free_page(spa.file_name);
	}
	return TID_ERROR;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process(void *args_)
{
	struct start_process_args *args = args_;
	char *file_name = args->file_name;
	struct intr_frame if_ = {0};
	bool success = false;

	/* Initialize interrupt frame and load executable. */
	memset(&if_, 0, sizeof if_);
	if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
	if_.cs = SEL_UCSEG;
	if_.eflags = FLAG_IF | FLAG_MBS;
	success = prepare_executable_and_arguments(&if_, file_name);

	if (success) {
		/* On successful load(), take str ownership. */
		palloc_free_page(file_name);
		/* Switch placeholder TID_ERROR to real tid_t value. */
		args->child_tid = thread_tid();
	}

	/* Signal completed load() and ready-to-read tid_t value. */
	sema_up(&args->child_ready);

	if (!success) {
		thread_exit(EXIT_NO_LOAD); /* If load failed, quit. */
	}

	/* Start the user process by simulating a return from an
	   interrupt, implemented by intr_exit (in
	   threads/intr-stubs.S).  Because intr_exit takes all of its
	   arguments on the stack in the form of a `struct intr_frame',
	   we just point the stack pointer (%esp) to our stack frame
	   and jump to it. */
	asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
	NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait(tid_t child)
{
	int result = -1;
	if (child == TID_ERROR) {
		return result;
	}

	struct thread *t = thread_current();
	lock_acquire(&t->wait.lock);

	while (true) {
		ASSERT(lock_held_by_current_thread(&t->wait.lock));

		struct thread_wait_code *got_child = NULL;
		struct list_elem *e = list_begin(&t->wait.children);
		for (; e != list_end(&t->wait.children); e = list_next(e)) {
			struct thread_wait_code *twc =
				list_entry(e, struct thread_wait_code, elem);
			if (twc->tid == child) {
				got_child = twc;
				break;
			}
		}

		if (got_child == NULL) {
			break; /* No matching child. Fail-fast. */
		}

		if (got_child->code != EXIT_UNSET) {
			/* Consume status code, and arrange for future,
			   duplicate invocations of wait() to fail. */
			result = got_child->code;
			list_remove(&got_child->elem);
			free(got_child);
			break;
		}

		/* Wait until matching child sets an exit code. */
		ASSERT(got_child->code == EXIT_UNSET);
		cond_wait(&t->wait.on_exit, &t->wait.lock);
	}

	lock_release(&t->wait.lock);
	return result;
}

/* Free the current process's resources. */
void
process_exit(int status)
{
	struct thread *cur = thread_current();
	uint32_t *pd;

	const bool early_error_in_load = (status == EXIT_NO_LOAD);
	status = early_error_in_load ? EXIT_EXCEPTION : status;

	/* Destroy the current process's page directory and switch back
	   to the kernel-only page directory. */
	pd = cur->pagedir;
	if (pd != NULL) {
		/* Correct ordering here is crucial.  We must set
		   cur->pagedir to NULL before switching page directories,
		   so that a timer interrupt can't switch back to the
		   process page directory.  We must activate the base page
		   directory before destroying the process's page
		   directory, or our active page directory will be one
		   that's been freed (and cleared). */
		cur->pagedir = NULL;
		pagedir_activate(NULL);
		pagedir_destroy(pd);
	}

	/* Close outstanding file descriptors. */
	while (!list_empty(&cur->fd_table)) {
		struct list_elem *e = list_pop_front(&cur->fd_table);
		struct fdtable_entry *fde =
			list_entry(e, struct fdtable_entry, elem);

		struct file *file = fde->file; /* Copy out before free(fde). */
		free(fde);
		fde = NULL;

		acquire_io_lock();
		file_close(file);
		release_io_lock();
	}

	/* Register status code with our parent, who may wait() on us. */
	thread_signal_exit(cur->wait.allowed_parent, cur->tid, status);

	/* Clear status codes of children that we never wait()-ed on. */
	lock_acquire(&cur->wait.lock);
	while (!list_empty(&cur->wait.children)) {
		struct list_elem *e = list_pop_front(&cur->wait.children);
		struct thread_wait_code *twc =
			list_entry(e, struct thread_wait_code, elem);
		free(twc);
	}
	lock_release(&cur->wait.lock);

	if (!early_error_in_load) {
		printf("%s: exit(%d)\n", cur->name, status);
	}
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate(void)
{
	struct thread *t = thread_current();

	/* Activate thread's page tables. */
	pagedir_activate(t->pagedir);

	/* Set thread's kernel stack for use in processing
	   interrupts. */
	tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
	unsigned char e_ident[16];
	Elf32_Half e_type;
	Elf32_Half e_machine;
	Elf32_Word e_version;
	Elf32_Addr e_entry;
	Elf32_Off e_phoff;
	Elf32_Off e_shoff;
	Elf32_Word e_flags;
	Elf32_Half e_ehsize;
	Elf32_Half e_phentsize;
	Elf32_Half e_phnum;
	Elf32_Half e_shentsize;
	Elf32_Half e_shnum;
	Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
	Elf32_Word p_type;
	Elf32_Off p_offset;
	Elf32_Addr p_vaddr;
	Elf32_Addr p_paddr;
	Elf32_Word p_filesz;
	Elf32_Word p_memsz;
	Elf32_Word p_flags;
	Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void **esp, void **kpage);
static bool validate_segment(const struct Elf32_Phdr *, struct file *);
static bool load_segment(struct file *file,
                         off_t ofs,
                         uint8_t *upage,
                         uint32_t read_bytes,
                         uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
static bool
load(const char *file_name, void (**eip)(void), void **esp, void **kpage)
{
	struct thread *t = thread_current();
	struct Elf32_Ehdr ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pagedir = pagedir_create();
	if (t->pagedir == NULL)
		goto done;
	process_activate();

	acquire_io_lock(); /* Synchronize filesys.h API usage. */

	/* Open executable file. */
	file = filesys_open(file_name);
	if (file == NULL) {
		printf("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
	    memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 ||
	    ehdr.e_machine != 3 || ehdr.e_version != 1 ||
	    ehdr.e_phentsize != sizeof(struct Elf32_Phdr) ||
	    ehdr.e_phnum > 1024) {
		printf("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Elf32_Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file)) {
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint32_t file_page = phdr.p_offset & ~PGMASK;
				uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint32_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0) {
					/* Normal segment.
					   Read initial part from disk and zero
					   the rest. */
					read_bytes =
						page_offset + phdr.p_filesz;
					zero_bytes =
						(ROUND_UP(page_offset +
					                          phdr.p_memsz,
					                  PGSIZE) -
					         read_bytes);
				} else {
					/* Entirely zero.
					   Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(
						page_offset + phdr.p_memsz,
						PGSIZE);
				}
				if (!load_segment(file,
				                  file_page,
				                  (void *)mem_page,
				                  read_bytes,
				                  zero_bytes,
				                  writable))
					goto done;
			} else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack(esp, kpage))
		goto done;

	/* Start address. */
	*eip = (void (*)(void))ehdr.e_entry;

	success = true;

	/* On successful load(), deny writes to executable file until exit(). */
	file_deny_write(file);
	fd_register(file);

done:
	/* We arrive here whether the load is successful or not. */
	release_io_lock();
	return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Elf32_Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (Elf32_Off)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment(struct file *file,
             off_t ofs,
             uint8_t *upage,
             uint32_t read_bytes,
             uint32_t zero_bytes,
             bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Calculate how to fill this page.
		   We will read PAGE_READ_BYTES bytes from FILE
		   and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes =
			read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get page of memory. Add it to the process's address space. */
		uint8_t *kpage = page_create(upage, writable);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read(file, kpage, page_read_bytes) !=
		    (int)page_read_bytes) {
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack(void **esp, void **kpage)
{
	ASSERT(kpage != NULL && *kpage == NULL);
	void *upage = PHYS_BASE - PGSIZE;

	*kpage = page_create_zero(upage, true);
	if (*kpage == NULL) {
		return false;
	}

	*esp = PHYS_BASE;
	return true;
}

struct stack_layout {
	size_t stack_start;
	size_t stack_pos_argc;
	size_t stack_pos_argv_ptr;
	size_t stack_pos_argv_arr;
	size_t stack_pos_argv_data;
};

static struct stack_layout
get_stack_layout(int argc, size_t command_size)
{
	size_t stack_usage = 0;
	stack_usage += sizeof(void (*)());
	stack_usage += sizeof(int);
	stack_usage += sizeof(char **);
	const size_t arr_usage = (argc + 1) * sizeof(char *);
	stack_usage += arr_usage;
	const size_t padding = ROUND_UP(command_size, 4) % 4;
	stack_usage += padding;
	stack_usage += command_size;

	ASSERT(stack_usage <= PGSIZE);

	struct stack_layout sl = {0};
	sl.stack_start = PGSIZE - stack_usage;
	sl.stack_pos_argc = sl.stack_start + sizeof(void (*)());
	sl.stack_pos_argv_ptr = sl.stack_pos_argc + sizeof(int);
	sl.stack_pos_argv_arr = sl.stack_pos_argv_ptr + sizeof(char **);
	sl.stack_pos_argv_data = sl.stack_pos_argv_arr + arr_usage + padding;
	return sl;
}

struct stack_arguments {
	int argc;
	char *argv[32]; /* Handle ARG_MAX of 32. */
	char *command;
	size_t command_size;
};

static void *
fill_stack_with_layout(const struct stack_layout *s,
                       const struct stack_arguments *a,
                       void *kpage)
{
	void *upage = PHYS_BASE - PGSIZE;

	memset(kpage + s->stack_start, 0, sizeof(void (*)())); /* return addr */
	memcpy(kpage + s->stack_pos_argc, &a->argc, sizeof(a->argc));
	{
		const void *p = upage + s->stack_pos_argv_ptr + sizeof(char **);
		memcpy(kpage + s->stack_pos_argv_ptr, &p, sizeof(char **));
	}
	for (int i = 0; i <= a->argc; ++i) {
		void *kpage_cursor =
			kpage + s->stack_pos_argv_arr + (i * sizeof(char *));
		if (i == a->argc) {
			memset(kpage_cursor, 0, sizeof(char *));
		} else {
			const char *p = upage + s->stack_pos_argv_data;
			p += (a->argv[i] - a->command);
			memcpy(kpage_cursor, &p, sizeof(char *));
		}
	}
	memcpy(kpage + s->stack_pos_argv_data, a->command, a->command_size);

	return upage + s->stack_start;
}

static bool
prepare_executable_and_arguments(struct intr_frame *if_, char *command)
{
	struct stack_arguments sa = {
		.argc = 0,
		.argv = {NULL},
		.command = command,
		.command_size = strlen(command) + 1, /* with null terminator */
	};
	ASSERT(sa.command_size <= PGSIZE);

	void *kpage = NULL;
	char *save_ptr = NULL;

	for (char *token = strtok_r(sa.command, " ", &save_ptr);
	     (token != NULL) && ((size_t)sa.argc < ARRAY_SIZE(sa.argv));
	     token = strtok_r(NULL, " ", &save_ptr), ++sa.argc) {
		if (kpage == NULL) {
			/* load() executable, including setup_stack() */
			if (!load(token, &if_->eip, &if_->esp, &kpage)) {
				return false;
			}
			ASSERT(kpage != NULL);
		}
		sa.argv[sa.argc] = token;
	}

	struct stack_layout sl = get_stack_layout(sa.argc, sa.command_size);
	if_->esp = fill_stack_with_layout(&sl, &sa, kpage);
	return true;
}
