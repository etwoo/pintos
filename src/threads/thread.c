#include "threads/thread.h"

#include "devices/timer.h" /* for TIMER_FREQ */
#include "filesys/directory.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/page.h"

#include <array.h>
#include <debug.h>
#include <limits.h> /* for INT_MAX */
#include <random.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* System load average. Must disable interrupts before accessing. */
static struct fix_t system_load_avg;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
	void *eip;             /* Return address. */
	thread_func *function; /* Function to call. */
	void *aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *running_thread(void);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void *alloc_frame(struct thread *, size_t size);
static void schedule(void);
void thread_schedule_tail(struct thread *prev);
static tid_t allocate_tid(void);
static void thread_update_load_avg(void);
static void thread_update_recent_cpu(void);
static int thread_get_priority_of_mlfqs(struct thread *t);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&all_list);
	system_load_avg = i32_to_fixed(0);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick(int timer_ticks_snapshot)
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pagedir != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	if (t != idle_thread) {
		t->recent_cpu = add_fixed_i32(t->recent_cpu, 1);
	}

	const bool once_per_second = (timer_ticks_snapshot % TIMER_FREQ == 0);
	if (thread_mlfqs && once_per_second) {
		thread_update_load_avg();
		thread_update_recent_cpu();
	}

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void
thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
	       idle_ticks,
	       kernel_ticks,
	       user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create(const char *name, int priority, thread_func *function, void *aux)
{
	struct thread *t;
	struct kernel_thread_frame *kf;
	struct switch_entry_frame *ef;
	struct switch_threads_frame *sf;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();
	t->wait.allowed_parent = thread_tid();

	/* Stack frame for kernel_thread(). */
	kf = alloc_frame(t, sizeof *kf);
	kf->eip = NULL;
	kf->function = function;
	kf->aux = aux;

	/* Stack frame for switch_entry(). */
	ef = alloc_frame(t, sizeof *ef);
	ef->eip = (void (*)(void))kernel_thread;

	/* Stack frame for switch_threads(). */
	sf = alloc_frame(t, sizeof *sf);
	sf->eip = switch_entry;
	sf->ebp = 0;

	/* Add to run queue. */
	thread_unblock(t);

	/* Yield, in case newly created thread donates priority. */
	thread_yield();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);

	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	list_push_back(&ready_list, &t->elem);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid(void)
{
	return thread_current()->tid;
}

static void
recall_donation_from_dying_thread(struct thread *t, void *aux)
{
	struct thread *dying = aux;
	for (size_t i = 0; i < ARRAY_SIZE(t->donate); ++i) {
		if (t->donate[i] == dying) {
			t->donate[i] = NULL;
		}
	}
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit(int status)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit(status);
#endif

	/* Remove thread from all threads list, set our status to dying,
	   and schedule another process.  That process will destroy us
	   when it calls thread_schedule_tail(). */
	intr_disable();
	struct thread *dying = thread_current();
	list_remove(&dying->allelem);
	if (!thread_mlfqs) {
		thread_foreach(recall_donation_from_dying_thread, dying);
	}
	dying->status = THREAD_DYING;
	schedule();
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield(void)
{
	struct thread *cur = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (cur != idle_thread)
		list_push_back(&ready_list, &cur->elem);
	cur->status = THREAD_READY;
	schedule();
	intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach(thread_action_func *func, void *aux)
{
	struct list_elem *e;

	ASSERT(intr_get_level() == INTR_OFF);

	for (e = list_begin(&all_list); e != list_end(&all_list);
	     e = list_next(e)) {
		struct thread *t = list_entry(e, struct thread, allelem);
		func(t, aux);
	}
}

/* Sets the current thread's priority to NEW_PRIORITY.
   If the current thread no longer has the highest priority, yields. */
void
thread_set_priority(int new_priority)
{
	ASSERT(!thread_mlfqs);
	ASSERT(PRI_MIN <= new_priority && new_priority <= PRI_MAX);
	thread_current()->priority = new_priority;
	thread_yield();
}

int
thread_get_priority_of(struct thread *t)
{
	ASSERT(is_thread(t));
	int result = PRI_DEFAULT;

	if (thread_mlfqs) {
		result = thread_get_priority_of_mlfqs(t);
	} else {
		result = t->priority;
		for (size_t i = 0; i < ARRAY_SIZE(t->donate); ++i) {
			struct thread *candidate = t->donate[i];
			if (candidate == NULL) {
				continue;
			}
			const int donated = thread_get_priority_of(candidate);
			if (donated > result) {
				result = donated;
			}
		}
	}

	ASSERT(result >= PRI_MIN && result <= PRI_MAX);
	return result;
}

/* Returns the current thread's priority.
   In the presence of donation, returns the higher (donated) priority. */
int
thread_get_priority(void)
{
	return thread_get_priority_of(thread_current());
}

/* Sets the current thread's nice value to NICE.
   If the current thread no longer has the highest priority, yields. */
void
thread_set_nice(int new_nice)
{
	ASSERT(thread_mlfqs);
	ASSERT(NICE_MIN <= new_nice && new_nice <= NICE_MAX);
	thread_current()->nice = new_nice;
	thread_yield();
}

/* Returns the current thread's nice value. */
int
thread_get_nice(void)
{
	ASSERT(thread_mlfqs);
	return thread_current()->nice;
}

static int
to_int_100x(struct fix_t x)
{
	int32_t n = fixed_to_i32_rounding_nearest(mul_fixed_i32(x, 100));
	ASSERT(INT_MIN <= n && n <= INT_MAX); /* safe to cast int32_t to int */
	return n;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg(void)
{
	ASSERT(thread_mlfqs);
	struct fix_t copy = {0};
	{
		enum intr_level old_level = intr_disable();
		copy = system_load_avg;
		intr_set_level(old_level);
	}
	return to_int_100x(copy);
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu(void)
{
	ASSERT(thread_mlfqs);
	return to_int_100x(thread_current()->recent_cpu);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;
	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */

	thread_exit(0); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread(void)
{
	uint32_t *esp;

	/* Copy the CPU's stack pointer into `esp', and then round that
	   down to the start of a page.  Because `struct thread' is
	   always at the beginning of a page and the stack pointer is
	   somewhere in the middle, this locates the curent thread. */
	asm("mov %%esp, %0" : "=g"(esp));
	return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
bool
is_thread(struct thread *t)
{
	return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	enum intr_level old_level;

	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(!thread_mlfqs ||
	       priority == PRI_DEFAULT || /* thread_mlfqs: most threads */
	       t->priority == PRI_MIN);   /* thread_mlfqs: idle_thread */
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->stack = (uint8_t *)t + PGSIZE;
	t->priority = priority;
	for (size_t i = 0; i < ARRAY_SIZE(t->donate); ++i) {
		ASSERT(t->donate[i] == NULL);
	}
	t->nice = NICE_DEFAULT;
	t->recent_cpu = i32_to_fixed(0);
	list_init(&t->fd_table);
	t->fd_generator = STDERR_FILENO + 1; /* first available fd value */
	t->wait.allowed_parent = TID_ERROR;
	lock_init(&t->wait.lock);
	cond_init(&t->wait.on_exit);
	list_init(&t->wait.children);
	lock_init(&t->vm.lock);
	t->vm.initialized = false;
	t->vm.mmap_generator = 1; /* first valid mapping ID */
	t->fs.cwd = NULL;
	t->magic = THREAD_MAGIC;

	old_level = intr_disable();
	list_push_back(&all_list, &t->allelem);
	intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame(struct thread *t, size_t size)
{
	/* Stack data is always allocated in word-size units. */
	ASSERT(is_thread(t));
	ASSERT(size % sizeof(uint32_t) == 0);

	t->stack -= size;
	return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	ASSERT(intr_get_level() == INTR_OFF);
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return thread_pop_by_priority(&ready_list);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail(struct thread *prev)
{
	struct thread *cur = running_thread();

	ASSERT(intr_get_level() == INTR_OFF);

	/* Mark us as running. */
	cur->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate();
#endif

	/* If the thread we switched from is dying, destroy its struct
	   thread.  This must happen late so that thread_exit() doesn't
	   pull out the rug under itself.  (We don't free
	   initial_thread because its memory was not obtained via
	   palloc().) */
	if (prev != NULL && prev->status == THREAD_DYING &&
	    prev != initial_thread) {
		ASSERT(prev != cur);
		palloc_free_page(prev);
	}
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule(void)
{
	struct thread *cur = running_thread();
	struct thread *next = next_thread_to_run();
	struct thread *prev = NULL;

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(cur->status != THREAD_RUNNING);
	ASSERT(is_thread(next));

	if (cur != next)
		prev = switch_threads(cur, next);
	thread_schedule_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

struct thread *
thread_pop_by_priority(struct list *threads)
{
	ASSERT(!list_empty(threads));
	int max_priority = PRI_MIN;
	bool max_donated = false;
	struct thread *result = NULL;

	struct list_elem *e = list_begin(threads);
	for (; e != list_end(threads); e = list_next(e)) {
		struct thread *candidate = list_entry(e, struct thread, elem);
		const int priority = thread_get_priority_of(candidate);
		const bool is_donated = (priority > candidate->priority);
		if (result == NULL ||
		    /* Choose the highest priority value, preferring the
		       earliest occurrence of each given value, which implicitly
		       leads to round-robin effects. */
		    priority > max_priority ||
		    /* If two threads tie on priority, prefer inline priority
		       over donated priority of the same scalar value. */
		    (priority == max_priority && max_donated && !is_donated)) {
			max_priority = priority;
			max_donated = is_donated;
			result = candidate;
		}
	}

	ASSERT(result != NULL);
	list_remove(&result->elem);
	return result;
}

static void
is_thread_ready(struct thread *t, void *aux)
{
	int32_t *ready_threads = aux;

	if (t == idle_thread) {
		return;
	}

	switch (t->status) {
	case THREAD_RUNNING:
	case THREAD_READY:
		(*ready_threads)++;
		break;
	case THREAD_BLOCKED:
	case THREAD_DYING:
		break;
	}
}

static void
thread_update_load_avg(void)
{
	ASSERT(thread_mlfqs);
	ASSERT(intr_context());
	ASSERT(intr_get_level() == INTR_OFF);

	int32_t ready_threads = 0;
	thread_foreach(is_thread_ready, &ready_threads);

	/* load_avg = (59/60)*load_avg + (1/60)*ready_threads */
	const struct fix_t load_avg_term =
		mul_fixed_fixed(div_fixed_i32(i32_to_fixed(59), 60),
	                        system_load_avg);
	const struct fix_t ready_term =
		mul_fixed_i32(div_fixed_i32(i32_to_fixed(1), 60),
	                      ready_threads);
	system_load_avg = add_fixed_fixed(load_avg_term, ready_term);
}

static void
update_recent_cpu(struct thread *t, void *aux)
{
	/* recent_cpu = ((2*load_avg) / (2*load_avg + 1)) * recent_cpu + nice */
	const struct fix_t coefficient = *((const struct fix_t *)aux);
	const struct fix_t base = mul_fixed_fixed(coefficient, t->recent_cpu);
	t->recent_cpu = add_fixed_i32(base, t->nice);
}

static void
thread_update_recent_cpu(void)
{
	ASSERT(thread_mlfqs);
	ASSERT(intr_context());
	ASSERT(intr_get_level() == INTR_OFF);

	const struct fix_t load_avg_2x = mul_fixed_i32(system_load_avg, 2);
	const struct fix_t coefficient =
		div_fixed_fixed(load_avg_2x, add_fixed_i32(load_avg_2x, 1));

	thread_foreach(update_recent_cpu, (void *)&coefficient);
}

static int
thread_get_priority_of_mlfqs(struct thread *t)
{
	ASSERT(thread_mlfqs);
	ASSERT(is_thread(t));
	ASSERT(t->priority == PRI_DEFAULT || t->priority == PRI_MIN);

	/* priority = PRI_MAX - (recent_cpu / 4) - (nice * 2) */
	const int32_t recent_cpu_term =
		fixed_to_i32_rounding_nearest(div_fixed_i32(t->recent_cpu, 4));
	const int32_t priority = PRI_MAX - recent_cpu_term - (t->nice * 2);

	/* Clamp to range [PRI_MIN, PRI_MAX]. */
	const int clamped =
		((priority <= PRI_MIN)
	                 ? PRI_MIN
	                 : ((priority >= PRI_MAX) ? PRI_MAX : priority));
	return clamped;
}

static void
thread_call_on_match(tid_t target,
                     struct lock *choose_lock(struct thread *),
                     void do_work_with_lock(struct thread *, void *),
                     void *aux)
{
	if (target == TID_ERROR) {
		return;
	}

	struct thread *t = NULL;
	enum intr_level old_level = intr_disable();
	{
		struct list_elem *e = list_begin(&all_list);
		for (; e != list_end(&all_list); e = list_next(e)) {
			struct thread *candidate =
				list_entry(e, struct thread, allelem);
			if (candidate->tid == target) {
				t = candidate;
				break;
			}
		}
	}
	if (t == NULL) {
		intr_set_level(old_level);
		return;
	}

	/* Acquire thread-level lock, and then restore interrupts. */
	lock_acquire(choose_lock(t));
	intr_set_level(old_level);

	do_work_with_lock(t, aux);

	lock_release(choose_lock(t));
}

static struct lock *
get_wait_lock(struct thread *t)
{
	return &t->wait.lock;
}

struct status_args {
	tid_t child;
	int child_status;
};

static void
set_child_status(struct thread *t, void *aux)
{
	ASSERT(lock_held_by_current_thread(&t->wait.lock));
	struct status_args *args = aux;

	struct list_elem *e = list_begin(&t->wait.children);
	for (; e != list_end(&t->wait.children); e = list_next(e)) {
		struct thread_wait_code *twc =
			list_entry(e, struct thread_wait_code, elem);
		if (twc->tid == args->child) {
			ASSERT(twc->code == EXIT_UNSET);
			/* Update entry registered by process_execute(). */
			twc->code = args->child_status;
			cond_broadcast(&t->wait.on_exit, &t->wait.lock);
			break;
		}
	}
}

void
thread_signal_exit(tid_t parent, tid_t child, int child_status)
{
	struct status_args args = {
		.child = child,
		.child_status = child_status,
	};
	thread_call_on_match(parent, get_wait_lock, set_child_status, &args);
}

#ifdef VM

static struct lock *
get_vm_lock(struct thread *t)
{
	return &t->vm.lock;
}

struct evict_args {
	void *upage;
	void *kpage_stolen;
};

/* Reach into page.c internals. */
extern void *page_evict_internal(struct thread *t, void *upage);

static void
page_evict_glue(struct thread *t, void *aux)
{
	struct evict_args *args = aux;
	args->kpage_stolen = page_evict_internal(t, args->upage);
}

void *
thread_page_evict(tid_t victim, void *upage)
{
	struct evict_args args = {
		.upage = upage,
		.kpage_stolen = NULL,
	};
	thread_call_on_match(victim, get_vm_lock, page_evict_glue, &args);
	return args.kpage_stolen;
}

struct is_accessed_args {
	void *upage;
	bool found_thread;
	bool is_accessed;
};

static void
page_is_accessed(struct thread *t, void *aux)
{
	ASSERT(lock_held_by_current_thread(&t->vm.lock));

	struct is_accessed_args *args = aux;
	args->found_thread = true;

	void *kpage = pagedir_get_page(t->pagedir, args->upage);
	args->is_accessed =
		/* Page accessed via user virtual address. */
		pagedir_is_accessed(t->pagedir, args->upage) ||
		/* Page accessed via kernel virtual address (alias). */
		(kpage != NULL && pagedir_is_accessed(t->pagedir, kpage));
	if (args->is_accessed) {
		/* Clear accessed bit on both aliases. */
		pagedir_set_accessed(t->pagedir, args->upage, false);
		if (kpage != NULL) {
			pagedir_set_accessed(t->pagedir, kpage, false);
		}
	}
}

enum thread_page
thread_page_is_accessed_test_and_set(tid_t tid, void *upage)
{
	struct is_accessed_args args = {
		.upage = upage,
		.found_thread = false,
		.is_accessed = false,
	};
	thread_call_on_match(tid, get_vm_lock, page_is_accessed, &args);
	if (!args.found_thread) {
		ASSERT(!args.is_accessed);
		return THREAD_PAGE_UNKNOWN;
	} else if (args.is_accessed) {
		return THREAD_PAGE_IS_ACCESSED;
	} else {
		return THREAD_PAGE_NOT_ACCESSED;
	}
}

#endif
