/*
HOST_WATCH.C

Write tracking for the guest's Xbox window, the interface of
port/linux/src/memory_watch.c: the renderer keeps copies of textures and
vertices that live in the window (xbox_textures.c, d3d8_gl.c) and asks
which pages were written since.

Linux and Android write-protect the pages they watch and catch the first
write's fault. A Switch homebrew program can catch a fault only where
Atmosphère lets user exception handlers return (its
enable_user_exception_handlers setting), so here no write faults:

- the writes the game announces mark their pages written at once: file
  reads into the window (xbox_files.c) and the D3D locks (d3d8_resources.c)
  call memory_watch_prepare_write;
- the others are found by comparison. Watching a page copies it into a
  shadow of the window; a thread compares watched pages with their copies,
  often for pages that changed lately ("hot") and ever less often (up to
  every 64 passes) for pages that keep their contents. Asking for a hot
  page's generation compares it at once, so dynamic data is never drawn
  stale.

A write the game does not announce to a page long unchanged can thus show
late, by up to about a second. debug.watch_verify (config.toml) compares
every watched page on every pass and logs each change the schedule would
have found late, to find the writes worth announcing.
*/

#include "host.h"

#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define PAGE 0x1000u
#define PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / PAGE)
/* passes between two comparisons of a page that keeps its contents */
#define LONGEST_INTERVAL 64
/* a pass a frame */
#define PASS_NANOSECONDS 16666667ll

enum
{
	_page_watched = 1,
	_page_hot = 2,
};

static Mutex watch_lock;
static uint8_t *shadow;
static uint8_t page_flags[PAGE_COUNT];
static uint8_t page_interval[PAGE_COUNT];
static uint32_t page_next_pass[PAGE_COUNT];
static uint32_t page_generation[PAGE_COUNT];
static uint32_t current_generation = 1;
static uint32_t watch_serial = 1;
static uint32_t pass;
static int verify;

static int page_range(uint32_t address, uint32_t size, uint32_t *first, uint32_t *last)
{
	uint64_t start = address, end = (uint64_t)address + size;

	if (!size || end <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return 0;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	if (end > (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		end = (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE;
	*first = (uint32_t)((start - HALO_GUEST_WINDOW_BASE) / PAGE);
	*last = (uint32_t)((end - 1 - HALO_GUEST_WINDOW_BASE) / PAGE);
	return 1;
}

static const void *page_address(uint32_t page)
{
	return (const void *)(uintptr_t)(HALO_GUEST_WINDOW_BASE + page * PAGE);
}

/* the page was written: a new generation, and no longer watched (the
renderer watches it again when it takes a new copy). Called with the lock */
static void page_written(uint32_t page)
{
	page_generation[page] = __atomic_add_fetch(&current_generation, 1, __ATOMIC_RELAXED);
	page_flags[page] = _page_hot;
	__atomic_add_fetch(&watch_serial, 1, __ATOMIC_RELEASE);
}

/* compares a watched page with its copy; 1 if it changed. Called with the
lock */
static int page_check(uint32_t page)
{
	if (!memcmp(page_address(page), shadow + (size_t)page * PAGE, PAGE))
		return 0;
	page_written(page);
	return 1;
}

/* ---------- the comparing thread */

static void *watch_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		uint32_t page;
		uint32_t late = 0;

		svcSleepThread(PASS_NANOSECONDS);
		mutexLock(&watch_lock);
		pass++;
		for (page = 0; page < PAGE_COUNT; page++)
		{
			if (!(page_flags[page] & _page_watched))
				continue;
			if ((int32_t)(pass - page_next_pass[page]) >= 0)
			{
				if (page_check(page))
					continue;
				if (page_interval[page] < LONGEST_INTERVAL)
					page_interval[page] *= 2;
				if (page_interval[page] > 1)
					page_flags[page] &= ~_page_hot;
				page_next_pass[page] = pass + page_interval[page];
			}
			else if (verify && page_check(page))
			{
				late++;
			}
			/* (let the guest's watch calls in now and then) */
			if (!(page % 4096))
			{
				mutexUnlock(&watch_lock);
				mutexLock(&watch_lock);
			}
		}
		mutexUnlock(&watch_lock);
		if (late)
			host_logf(HOST_LOG_WARN, "write tracking: %u pages changed between their comparisons", late);
	}
	return NULL;
}

void host_watch_start(void)
{
	mutexInit(&watch_lock);
	verify = host_config_boolean("debug.watch_verify");
}

/* ---------- the guest's interface (memory_watch.c) */

void host_memory_watch_initialize(void)
{
	static int started;

	if (started)
		return;
	shadow = malloc(HALO_GUEST_WINDOW_SIZE);
	if (!shadow)
		host_fatal("There is not enough memory for the game's write tracking.");
	if (host_native_thread_create(watch_thread, NULL, 64 * 1024, _host_thread_background) != 0)
		host_fatal("Cannot start the write tracking thread.");
	started = 1;
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint32_t first, last, page;

	if (!shadow || !page_range(address, size, &first, &last))
		return;
	mutexLock(&watch_lock);
	for (page = first; page <= last; page++)
	{
		if (page_flags[page] & _page_watched)
			continue;
		memcpy(shadow + (size_t)page * PAGE, page_address(page), PAGE);
		page_flags[page] |= _page_watched;
		page_interval[page] = 1;
		page_next_pass[page] = pass + 1;
	}
	mutexUnlock(&watch_lock);
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint32_t first, last, page, newest = 0;
	int locked = 0;

	if (!page_range(address, size, &first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		/* a hot page is compared now: dynamic data changes every frame */
		if ((page_flags[page] & (_page_watched | _page_hot)) == (_page_watched | _page_hot))
		{
			if (!locked)
			{
				mutexLock(&watch_lock);
				locked = 1;
			}
			if ((page_flags[page] & _page_watched))
				page_check(page);
		}
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	if (locked)
		mutexUnlock(&watch_lock);
	return newest;
}

uint32_t host_memory_watch_serial(void)
{
	return __atomic_load_n(&watch_serial, __ATOMIC_ACQUIRE);
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint32_t first, last, page;

	if (!page_range(address, size, &first, &last))
		return;
	mutexLock(&watch_lock);
	for (page = first; page <= last; page++)
		page_written(page);
	mutexUnlock(&watch_lock);
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	host_memory_watch_prepare_write(address, size);
}
