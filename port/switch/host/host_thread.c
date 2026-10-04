/*
HOST_THREAD.C

Threads that run guest code.

Guest (ILP32) code keeps stack addresses in 32-bit registers, so every
thread that runs it needs its stack below 4 GB. libnx threads get stacks
the kernel places far above (it aliases the memory a thread is given into
its stack region), so a guest thread starts on a small libnx stack and at
once switches to one in the arena (call_on_stack), where the guest code and
the host functions it calls then run.

Horizon schedules strictly by priority: a ready thread never yields to one
of lower priority, and threads of the same priority take turns only when
they block or yield, but at priority 0x3b, where the kernel rotates them.
The game busy-waits here and there (it was written for the Xbox's
preemptive scheduler), so:

- the game's main thread has core 0 to itself (the libnx main thread);
- the guest's other threads run at 0x3b on cores 1 and 2, each core's in
  turn;
- the audio mixer runs above them on core 2.

The guest's thread pointer (its musl struct pthread) is kept for each
thread in host TLS.
*/

#include "host.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define GUARD_SIZE 0x4000
#define LIBNX_STACK_SIZE 0x8000
#define PREEMPTIVE_PRIORITY 0x3b
#define AUDIO_PRIORITY 0x2b

static __thread uint32_t guest_tp;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

/* uint64_t call_on_stack(void *(*function)(void *), void *argument, void *stack_top):
calls the function with the stack pointer at stack_top, and returns its
result on the caller's stack. The frame chain starts over there (a null
frame pointer): guest code walks it (its assertions' stack dumps), and a
frame record above 4 GB would reach it truncated */
uint64_t call_on_stack(void *(*function)(void *), void *argument, void *stack_top);
__asm__(
	".text\n"
	".global call_on_stack\n"
	".type call_on_stack, %function\n"
	"call_on_stack:\n"
	"	stp x29, x30, [sp, #-16]!\n"
	"	mov x29, sp\n"
	"	mov x3, sp\n"
	"	and x2, x2, #~15\n"
	"	mov sp, x2\n"
	"	str x3, [sp, #-16]!\n"
	"	mov x3, x0\n"
	"	mov x0, x1\n"
	"	mov x29, xzr\n"
	"	blr x3\n"
	"	ldr x3, [sp], #16\n"
	"	mov sp, x3\n"
	"	ldp x29, x30, [sp], #16\n"
	"	ret\n"
	".size call_on_stack, . - call_on_stack\n");

/* ---------- stacks */

struct guest_stack
{
	void *mapping;
	size_t size;
};

/* a stack in the arena with a guard below it; its top */
static void *stack_allocate(size_t size, struct guest_stack *stack)
{
	size = (size + 0xffff) & ~(size_t)0xffff;
	stack->size = size + GUARD_SIZE;
	stack->mapping = host_low_map(stack->size, 1);
	if (!stack->mapping)
		return NULL;
	svcSetMemoryPermission(stack->mapping, GUARD_SIZE, Perm_None);
	return (char *)stack->mapping + stack->size;
}

static int on_guest_stack(void)
{
	return (uint64_t)__builtin_frame_address(0) < 0x100000000ull;
}

/* ---------- calling into the guest */

typedef uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code was called on a thread without a guest stack");
	if (!guest_tp)
		((guest_function)(uintptr_t)host_image.header->thread_attach)(0, 0, 0, 0);
	return ((guest_function)(uintptr_t)function)(a, b, c, d);
}

void host_run_guest_main(uint32_t boot)
{
	((void (*)(uint32_t))(uintptr_t)host_image.header->start)(boot);
	host_fatal("the game returned from its start");
}

void host_thread_run_on_guest_stack(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct guest_stack stack;
	void *top = stack_allocate(stack_size, &stack);

	if (!top)
		host_fatal("There is no memory for the game's stack.");
	call_on_stack(function, argument, top);
	host_low_unmap(stack.mapping, stack.size);
}

/* ---------- threads */

struct thread_start
{
	Thread thread;
	void *(*function)(void *);
	void *argument;
	struct guest_stack stack;
	struct thread_start *next_finished;
};

static Mutex reaper_lock;
static CondVar reaper_condition;
static struct thread_start *finished_threads;
static Thread reaper;
static int reaper_started;

/* joins finished threads and frees their stacks (a thread cannot free the
stack it runs on) */
static void reaper_main(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct thread_start *finished;

		mutexLock(&reaper_lock);
		while (!finished_threads)
			condvarWait(&reaper_condition, &reaper_lock);
		finished = finished_threads;
		finished_threads = finished->next_finished;
		mutexUnlock(&reaper_lock);
		threadWaitForExit(&finished->thread);
		threadClose(&finished->thread);
		host_low_unmap(finished->stack.mapping, finished->stack.size);
		free(finished);
	}
}

static void thread_main(void *context)
{
	struct thread_start *start = context;

	call_on_stack(start->function, start->argument, (char *)start->stack.mapping + start->stack.size);
	guest_tp = 0;
	mutexLock(&reaper_lock);
	start->next_finished = finished_threads;
	finished_threads = start;
	condvarWakeOne(&reaper_condition);
	mutexUnlock(&reaper_lock);
}

static void role_placement(enum host_thread_role role, int *priority, int *core, uint32_t *core_mask)
{
	static uint32_t next_guest_core;

	switch (role)
	{
	case _host_thread_audio:
		*priority = AUDIO_PRIORITY;
		*core = 2;
		*core_mask = 1u << 2;
		break;
	case _host_thread_background:
		*priority = PREEMPTIVE_PRIORITY;
		*core = 2;
		*core_mask = (1u << 1) | (1u << 2);
		break;
	default:
		*priority = PREEMPTIVE_PRIORITY;
		*core = 1 + (int)(__atomic_fetch_add(&next_guest_core, 1, __ATOMIC_RELAXED) % 2);
		*core_mask = (1u << 1) | (1u << 2);
		break;
	}
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size,
	enum host_thread_role role)
{
	struct thread_start *start = calloc(1, sizeof(*start));
	uint32_t core_mask;
	int priority, core;
	Result result;

	if (!start)
		return ENOMEM;
	mutexLock(&reaper_lock);
	if (!reaper_started)
	{
		condvarInit(&reaper_condition);
		if (R_SUCCEEDED(threadCreate(&reaper, reaper_main, NULL, NULL, LIBNX_STACK_SIZE, PREEMPTIVE_PRIORITY, 2)) &&
			R_SUCCEEDED(threadStart(&reaper)))
			reaper_started = 1;
	}
	mutexUnlock(&reaper_lock);
	if (!stack_allocate(stack_size, &start->stack))
	{
		free(start);
		return EAGAIN;
	}
	start->function = function;
	start->argument = argument;
	role_placement(role, &priority, &core, &core_mask);
	result = threadCreate(&start->thread, thread_main, start, NULL, LIBNX_STACK_SIZE, priority, core);
	if (R_SUCCEEDED(result))
	{
		/* free to move between the cores it may use (a yield moves it) */
		svcSetThreadCoreMask(start->thread.handle, core, core_mask);
		result = threadStart(&start->thread);
		if (R_FAILED(result))
			threadClose(&start->thread);
	}
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "threadCreate: 0x%x", result);
		host_low_unmap(start->stack.mapping, start->stack.size);
		free(start);
		return EAGAIN;
	}
	return 0;
}

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, stack_size,
		_host_thread_guest);
}
