/*
HOST_PROFILE.C

A sampling profiler of the game's main thread, where frames are made: a
thread pauses it about a thousand times a second, reads where it is
(svcGetThreadContext3) and lets it go on. Every 10 seconds the log gets
the addresses it was found at most, in 16-byte buckets, for
llvm-symbolizer: below 4 GB, the guest image (build/switch/halo_guest.elf,
at its own addresses); above, the host program (build/switch/halo.elf, at
an offset from the base logged with them). Samples of the thread in a
system call (waiting for the GPU, a lock, the vertical blank) cannot be
read and are counted as "waiting".

On unless debug.profile = false in config.toml.
*/

#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define SAMPLE_NANOSECONDS 1000000ll
#define REPORT_NANOSECONDS 10000000000ull
#define BUCKETS 8192
#define REPORTED 24

struct bucket
{
	uint64_t address;
	uint32_t count;
};

static struct bucket buckets[BUCKETS];
static Handle sampled;
static Thread profiler;

static void count(uint64_t address)
{
	uint64_t key = address >> 4;
	uint32_t index = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 51) % BUCKETS;
	uint32_t probe;

	for (probe = 0; probe < 64; probe++)
	{
		struct bucket *bucket = &buckets[(index + probe) % BUCKETS];

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

static int by_count(const void *a, const void *b)
{
	const struct bucket *x = a, *y = b;

	return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void report(uint32_t samples, uint32_t waiting)
{
	extern int main(int argc, char *argv[]);
	struct bucket *sorted = malloc(sizeof(buckets));
	char line[1024];
	int index, length = 0;

	if (!sorted)
		return;
	memcpy(sorted, buckets, sizeof(buckets));
	qsort(sorted, BUCKETS, sizeof(*sorted), by_count);
	host_logf(HOST_LOG_INFO, "profile: %u samples, %u%% waiting in the kernel; host main at %lx", samples,
		samples ? waiting * 100 / samples : 0, (unsigned long)(uintptr_t)main);
	for (index = 0; index < REPORTED && sorted[index].count; index++)
	{
		length += snprintf(line + length, sizeof(line) - (size_t)length, " %lx:%u",
			(unsigned long)(sorted[index].address << 4), sorted[index].count);
		if (length > 900 || index % 8 == 7)
		{
			host_logf(HOST_LOG_INFO, "profile:%s", line);
			length = 0;
		}
	}
	if (length)
		host_logf(HOST_LOG_INFO, "profile:%s", line);
	free(sorted);
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
				count(context.pc.x);
			else
				waiting++;
		}
		now = armTicksToNs(armGetSystemTick());
		if (now - period_start >= REPORT_NANOSECONDS)
		{
			report(samples, waiting);
			memset(buckets, 0, sizeof(buckets));
			samples = waiting = 0;
			period_start = now;
		}
	}
}

void host_profile_start(void)
{
	if (!host_config_boolean_default("debug.profile", 1))
		return;
	sampled = envGetMainThreadHandle();
	/* (on core 1: the main thread's core is its own) */
	if (R_SUCCEEDED(threadCreate(&profiler, profiler_main, NULL, NULL, 0x10000, 0x2c, 1)) &&
		R_SUCCEEDED(threadStart(&profiler)))
		host_logf(HOST_LOG_INFO, "profiler on (debug.profile = false in config.toml turns it off)");
}
