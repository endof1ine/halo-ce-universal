/*
HOST_PROFILE.C

A sampling profiler of the game's main thread, where frames are made: a
thread pauses it about a thousand times a second, reads where it is
(svcGetThreadContext3) and lets it go on. A sample in the host's code (a
GL call, a system call) is also counted against the guest function that
called the host: the first guest return address on the thread's stack. Every 10 seconds the log gets
the addresses it was found at most, in 16-byte buckets, for
llvm-symbolizer: below 4 GB, the guest image (build/switch/halo_guest.elf,
at its own addresses); above, the host program (build/switch/halo.elf, at
an offset from the base logged with them). Samples of the thread in a
system call (waiting for the GPU, a lock, the vertical blank) cannot be
read and are counted as "waiting".

On in debug builds unless debug.profile = false in config.toml; in release
builds only with debug.profile = true.
*/

#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define SAMPLE_NANOSECONDS 2000000ll
#define REPORT_NANOSECONDS 10000000000ull
#define BUCKETS 8192
#define REPORTED 128

struct bucket
{
	uint64_t address;
	uint32_t count;
};

static struct bucket buckets[BUCKETS];
/* host samples by the guest code that called the host */
static struct bucket callers[BUCKETS];
static Handle sampled;
static Thread profiler;

/* (by instruction: a coarser bucket straddles functions, a system call's
stub and the next one, and is named after the first) */
static void count_in(struct bucket *table, uint64_t address)
{
	uint64_t key = address >> 2;
	uint32_t index = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 51) % BUCKETS;
	uint32_t probe;

	for (probe = 0; probe < 64; probe++)
	{
		struct bucket *bucket = &table[(index + probe) % BUCKETS];

		if (bucket->count && bucket->address == key)
		{
			bucket->count++;
			return;
		}
		if (!bucket->count)
		{
			bucket->address = key;
			bucket->count = 1;
			return;
		}
	}
}

/* a guest return address: in its code, after a BL or BLR */
static int guest_return_address(uint64_t value)
{
	uint32_t instruction;

	/* (the guest's code: host_loader.c's image) */
	if (value < (uint64_t)host_image.base + 4 || value >= host_image.code_end || (value & 3))
		return 0;
	instruction = *(const uint32_t *)(uintptr_t)(value - 4);
	return (instruction & 0xfc000000u) == 0x94000000u || (instruction & 0xfffffc1fu) == 0xd63f0000u;
}

/* the guest code the paused thread is in the host for: its link register,
or the first guest return address on its stack (the guest's import stubs
jump to the host, so a host function returns straight to the guest) */
static uint64_t guest_caller(const ThreadContext *context)
{
	const uint64_t *word = (const uint64_t *)(uintptr_t)context->sp;
	MemoryInfo block;
	u32 page_info;
	uint64_t count, index;

	if (guest_return_address(context->lr))
		return context->lr;
	/* (within the stack's memory block, readable) */
	if (context->sp >= 0x100000000ull || R_FAILED(svcQueryMemory(&block, &page_info, context->sp)) ||
		!(block.perm & Perm_R))
		return 0;
	count = (block.addr + block.size - context->sp) / sizeof(*word);
	if (count > 1024)
		count = 1024;
	for (index = 0; index < count; index++)
	{
		if (guest_return_address(word[index]))
			return word[index];
	}
	return 0;
}

static int by_count(const void *a, const void *b)
{
	const struct bucket *x = a, *y = b;

	return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void report_table(const struct bucket *table, const char *tag)
{
	struct bucket *sorted = malloc(sizeof(buckets));
	char line[1024];
	int index, length = 0;

	if (!sorted)
		return;
	memcpy(sorted, table, sizeof(buckets));
	qsort(sorted, BUCKETS, sizeof(*sorted), by_count);
	for (index = 0; index < REPORTED && sorted[index].count; index++)
	{
		length += snprintf(line + length, sizeof(line) - (size_t)length, " %lx:%u",
			(unsigned long)(sorted[index].address << 2), sorted[index].count);
		if (length > 900 || index % 8 == 7)
		{
			host_logf(HOST_LOG_INFO, "%s:%s", tag, line);
			length = 0;
		}
	}
	if (length)
		host_logf(HOST_LOG_INFO, "%s:%s", tag, line);
	free(sorted);
}

static void report(uint32_t samples, uint32_t waiting)
{
	extern int main(int argc, char *argv[]);

	host_logf(HOST_LOG_INFO, "profile: %u samples, %u%% waiting in the kernel; host main at %lx", samples,
		samples ? waiting * 100 / samples : 0, (unsigned long)(uintptr_t)main);
	report_table(buckets, "profile");
	report_table(callers, "profile callers");
}

static void profiler_main(void *unused)
{
	uint64_t period_start = armTicksToNs(armGetSystemTick());
	uint32_t samples = 0, waiting = 0;

	(void)unused;
	for (;;)
	{
		ThreadContext context;
		uint64_t now;

		svcSleepThread(SAMPLE_NANOSECONDS);
		if (R_SUCCEEDED(svcSetThreadActivity(sampled, ThreadActivity_Paused)))
		{
			Result result = svcGetThreadContext3(&context, sampled);

			svcSetThreadActivity(sampled, ThreadActivity_Runnable);
			samples++;
			if (R_SUCCEEDED(result))
			{
				count_in(buckets, context.pc.x);
				if (context.pc.x >= 0x100000000ull)
				{
					uint64_t caller = guest_caller(&context);

					count_in(callers, caller ? caller : 1);
				}
			}
			else
			{
				waiting++;
			}
		}
		now = armTicksToNs(armGetSystemTick());
		if (now - period_start >= REPORT_NANOSECONDS)
		{
			report(samples, waiting);
			memset(buckets, 0, sizeof(buckets));
			memset(callers, 0, sizeof(callers));
			samples = waiting = 0;
			period_start = now;
		}
	}
}

void host_profile_start(void)
{
#ifdef HALO_RELEASE
	if (!host_config_boolean_default("debug.profile", 0))
#else
	if (!host_config_boolean_default("debug.profile", 1))
#endif
		return;
	sampled = envGetMainThreadHandle();
	/* (on core 1: the main thread's core is its own) */
	if (R_SUCCEEDED(threadCreate(&profiler, profiler_main, NULL, NULL, 0x10000, 0x2c, 1)) &&
		R_SUCCEEDED(threadStart(&profiler)))
		host_logf(HOST_LOG_INFO, "profiler on (debug.profile = false in config.toml turns it off)");
}
