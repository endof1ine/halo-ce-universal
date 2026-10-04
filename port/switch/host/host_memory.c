/*
HOST_MEMORY.C

The guest's memory, all of it below 4 GB:

- the Xbox window, 0x80000000-0x88000000, whose addresses the game data
  holds: committed whole at start-up;
- the guest image above it (host_loader.c);
- the Custom Edition tag cache at 0x40440000, which that format's maps are
  linked to: committed when the game asks for it;
- an arena from 0x90000000 to 4 GB for everything else (the guest's malloc,
  thread stacks), committed in 2 MB chunks as it fills.

Horizon gives a homebrew program one way to put memory at an address of its
choosing: svcMapProcessCodeMemory, which maps heap memory there (as the
system's loader maps programs). The memory starts as "code" (execute and
read only once it was writable: the image's code is filled before it is
mapped); once made writable, svcSetMemoryPermission moves it between no
access, read and read-write, which is how the guest's mprotect is served.

Nothing is decommitted: the heap behind a mapping stays lent to it until the
process ends (host_exit).
*/

#include "host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define PAGE 0x1000ull
#define LOW_LIMIT 0x100000000ull
#define WINDOW_BASE ((uint64_t)HALO_GUEST_WINDOW_BASE)
#define WINDOW_END (WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
/* room for the image (it ends near 0x891a0000) */
#define IMAGE_BASE ((uint64_t)HALO_GUEST_IMAGE_BASE)
#define IMAGE_END 0x8c000000ull
/* port/linux/game/cache_file_formats.h: CUSTOM_EDITION_TAG_CACHE_BASE and
its upgraded size */
#define CUSTOM_EDITION_BASE 0x40440000ull
#define CUSTOM_EDITION_END (CUSTOM_EDITION_BASE + 0x2280000ull)
/* the arena: 0x90000000 to 4 GB where it is free, else the largest free
range above the image of at least ARENA_MINIMUM (the system lays out each
process at random: its own regions are sometimes in the way); at most
ARENA_MAXIMUM. Never below 0x80000000: the platform layer's
PLATFORM_PHYSICAL_TO_VIRTUAL sets that bit in the pointers it is given, the
arena's among them */
#define ARENA_PREFERRED 0x90000000ull
#define ARENA_MAXIMUM (LOW_LIMIT - ARENA_PREFERRED)
#define ARENA_MINIMUM 0x20000000ull
#define ARENA_LOWEST IMAGE_END
#define CHUNK 0x200000ull
#define ARENA_MAXIMUM_PAGES (ARENA_MAXIMUM / PAGE)
#define ARENA_MAXIMUM_CHUNKS (ARENA_MAXIMUM / CHUNK)
#define ARENA_BASE arena_base
#define ARENA_END arena_end
#define ARENA_PAGES ((arena_end - arena_base) / PAGE)

/* the guest's (Linux's) mmap arguments */
#define GUEST_PROT_READ 0x1
#define GUEST_PROT_WRITE 0x2
#define GUEST_MAP_FIXED 0x10
#define GUEST_MAP_ANONYMOUS 0x20
#define GUEST_MAP_FIXED_NOREPLACE 0x100000

static Mutex memory_lock;
static uint64_t arena_base = ARENA_PREFERRED, arena_end = LOW_LIMIT;
static uint64_t arena_used[ARENA_MAXIMUM_PAGES / 64];
static uint8_t arena_chunk_committed[ARENA_MAXIMUM_CHUNKS];
static uint64_t arena_hint;
static int custom_edition_committed;

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

static uint64_t round_up(uint64_t size)
{
	return (size + PAGE - 1) & ~(PAGE - 1);
}

/* ---------- committing */

/* lends heap memory to [address, address + size) as code memory, filled
with data (or zero), then gives it the permission */
static int commit(uint64_t address, uint64_t size, const void *data, uint64_t data_size, uint32_t permission)
{
	void *backing = aligned_alloc(PAGE, size);
	Result result;

	if (!backing)
	{
		host_logf(HOST_LOG_ERROR, "no heap to commit %llu KB at %08llx", (unsigned long long)(size >> 10),
			(unsigned long long)address);
		return -1;
	}
	if (data)
	{
		memcpy(backing, data, data_size);
		memset((char *)backing + data_size, 0, size - data_size);
	}
	else
	{
		memset(backing, 0, size);
	}
	armDCacheFlush(backing, size);
	result = svcMapProcessCodeMemory(envGetOwnProcessHandle(), address, (u64)backing, size);
	if (R_SUCCEEDED(result))
		result = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), address, size, permission);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "cannot map %llu KB at %08llx: 0x%x", (unsigned long long)(size >> 10),
			(unsigned long long)address, result);
		return -1;
	}
	if (permission == Perm_Rx)
		armICacheInvalidate((void *)address, size);
	return 0;
}

/* the guest's protection as a svcSetMemoryPermission one (never
executable: the guest only runs its image) */
static uint32_t permission_of(int protection)
{
	if (protection & GUEST_PROT_WRITE)
		return Perm_Rw;
	if (protection & GUEST_PROT_READ)
		return Perm_R;
	return Perm_None;
}

/* svcSetMemoryPermission takes only a range whose pages share one state
and permission (the kernel's memory blocks): a range is set block by block,
leaving those that already have the permission */
static int set_permission(uint64_t address, uint64_t size, uint32_t permission)
{
	uint64_t end = address + size;

	while (address < end)
	{
		MemoryInfo block;
		u32 page_info;
		uint64_t piece;
		Result result;

		if (R_FAILED(svcQueryMemory(&block, &page_info, address)))
			return -1;
		piece = block.addr + block.size - address;
		if (piece > end - address)
			piece = end - address;
		if (block.perm != permission)
		{
			result = svcSetMemoryPermission((void *)address, piece, permission);
			if (R_FAILED(result))
			{
				host_logf(HOST_LOG_ERROR, "svcSetMemoryPermission(%08llx, %llx, %u) in a block of type %u, "
					"permission %u, attributes %u: 0x%x", (unsigned long long)address, (unsigned long long)piece,
					permission, block.type, block.perm, block.attr, result);
				return -1;
			}
		}
		address += piece;
	}
	return 0;
}

/* fresh pages, as Linux gives them: zero, then with the permission */
static int fresh_pages(uint64_t address, uint64_t size, uint32_t permission)
{
	if (set_permission(address, size, Perm_Rw) != 0)
		return -1;
	memset((void *)address, 0, size);
	return permission == Perm_Rw ? 0 : set_permission(address, size, permission);
}

/* ---------- start-up */

static const char *check_free(uint64_t base, uint64_t end, const char *what)
{
	static const InfoType regions[][2] = {
		{InfoType_HeapRegionAddress, InfoType_HeapRegionSize},
		{InfoType_AliasRegionAddress, InfoType_AliasRegionSize},
		{InfoType_StackRegionAddress, InfoType_StackRegionSize},
	};
	static char reason[192];
	uint64_t address = base;
	unsigned index;

	for (index = 0; index < sizeof(regions) / sizeof(*regions); index++)
	{
		u64 start = 0, size = 0;

		svcGetInfo(&start, regions[index][0], CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&size, regions[index][1], CUR_PROCESS_HANDLE, 0);
		if (start < end && base < start + size)
		{
			host_logf(HOST_LOG_WARN, "%s: the system's region %u is at %010llx-%010llx", what, index,
				(unsigned long long)start, (unsigned long long)(start + size));
			goto taken;
		}
	}
	while (address < end)
	{
		MemoryInfo memory;
		u32 page_info;

		if (R_FAILED(svcQueryMemory(&memory, &page_info, address)) || memory.type != MemType_Unmapped)
		{
			host_logf(HOST_LOG_WARN, "%s: memory of type 0x%x at %010llx-%010llx", what, memory.type,
				(unsigned long long)memory.addr, (unsigned long long)(memory.addr + memory.size));
			goto taken;
		}
		address = memory.addr + memory.size;
	}
	return NULL;

taken:
	snprintf(reason, sizeof(reason),
		"The memory the game needs for its %s (%08llx-%08llx) is in use this time.\n\n"
		"Close the game you started the Homebrew Menu from, and start it again.",
		what, (unsigned long long)base, (unsigned long long)end);
	return reason;
}

/* the system's own regions (heap, alias, stack): unmapped, yet not free */
static int in_system_region(uint64_t base, uint64_t end, uint64_t *region_end)
{
	static const InfoType regions[][2] = {
		{InfoType_HeapRegionAddress, InfoType_HeapRegionSize},
		{InfoType_AliasRegionAddress, InfoType_AliasRegionSize},
		{InfoType_StackRegionAddress, InfoType_StackRegionSize},
	};
	unsigned index;

	for (index = 0; index < sizeof(regions) / sizeof(*regions); index++)
	{
		u64 start = 0, size = 0;

		svcGetInfo(&start, regions[index][0], CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&size, regions[index][1], CUR_PROCESS_HANDLE, 0);
		if (size && start < end && base < start + size)
		{
			*region_end = start + size;
			return 1;
		}
	}
	return 0;
}

/* the arena's range: the preferred one if free, else the largest free run
from ARENA_LOWEST to 4 GB, by chunks, outside the system's regions; 0 if
none is ARENA_MINIMUM */
static int arena_choose(void)
{
	uint64_t address = ARENA_LOWEST, best_base = 0, best_size = 0, run_base = 0;
	int in_run = 0;

	if (!check_free(ARENA_PREFERRED, LOW_LIMIT, "other memory"))
		return 1;
	while (address < LOW_LIMIT)
	{
		MemoryInfo memory;
		u32 page_info;
		uint64_t next, region_end;
		int usable;

		if (R_FAILED(svcQueryMemory(&memory, &page_info, address)))
			break;
		/* (a chunk at a time: free, and none of the fixed ranges') */
		next = address + CHUNK;
		usable = memory.type == MemType_Unmapped && memory.addr + memory.size >= next &&
			!(address < IMAGE_END && WINDOW_BASE < next) &&
			!(address < CUSTOM_EDITION_END && CUSTOM_EDITION_BASE < next) &&
			!in_system_region(address, next, &region_end);
		if (usable && !in_run)
		{
			run_base = address;
			in_run = 1;
		}
		if ((!usable || next >= LOW_LIMIT) && in_run)
		{
			uint64_t run_end = usable ? next : address;

			if (run_end - run_base > best_size)
			{
				best_base = run_base;
				best_size = run_end - run_base;
			}
			in_run = 0;
		}
		address = next;
	}
	if (best_size < ARENA_MINIMUM)
		return 0;
	if (best_size > ARENA_MAXIMUM)
		best_size = ARENA_MAXIMUM;
	arena_base = best_base;
	arena_end = best_base + best_size;
	host_logf(HOST_LOG_WARN, "the other memory at %08llx-%08llx, 0x90000000 being in use this time",
		(unsigned long long)arena_base, (unsigned long long)arena_end);
	return 1;
}

const char *host_memory_initialize(void)
{
	const char *reason;
	AppletType type = appletGetAppletType();

	mutexInit(&memory_lock);
	if (type != AppletType_Application && type != AppletType_SystemApplication)
		return "Halo needs the memory of a game.\n\n"
			"Start the Homebrew Menu from a game: hold R while you start any game, "
			"then start Halo. (From the Album, the Homebrew Menu has too little memory.)";
	if (envGetOwnProcessHandle() == INVALID_HANDLE)
		return "This Homebrew Menu does not give its programs their process handle. Update the Homebrew Menu "
			"(hbmenu and hbloader).";
	if ((reason = check_free(WINDOW_BASE, WINDOW_END, "Xbox memory")) ||
		(reason = check_free(IMAGE_BASE, IMAGE_END, "program")) ||
		(reason = check_free(CUSTOM_EDITION_BASE, CUSTOM_EDITION_END, "Custom Edition maps")))
		return reason;
	if (!arena_choose())
		return check_free(ARENA_PREFERRED, LOW_LIMIT, "other memory");
	if (commit(WINDOW_BASE, WINDOW_END - WINDOW_BASE, NULL, 0, Perm_Rw) != 0)
		return "There is not enough memory for the game. Close other programs and try again.";
	host_logf(HOST_LOG_INFO, "Xbox window %08llx-%08llx committed", (unsigned long long)WINDOW_BASE,
		(unsigned long long)WINDOW_END);
	return NULL;
}

int host_memory_map_code(uint32_t address, const void *data, uint32_t size)
{
	uint64_t length = round_up(size);

	if (!in_range(address, length, IMAGE_BASE, IMAGE_END))
		return -1;
	return commit(address, length, data, size, Perm_Rx);
}

int host_memory_map_data(uint32_t address, uint32_t size)
{
	uint64_t length = round_up(size);

	if (!in_range(address, length, IMAGE_BASE, IMAGE_END))
		return -1;
	return commit(address, length, NULL, 0, Perm_Rw);
}

/* ---------- the arena */

static int page_used(uint64_t page)
{
	return (int)((arena_used[page / 64] >> (page % 64)) & 1);
}

static void pages_mark(uint64_t first, uint64_t count, int used)
{
	uint64_t page;

	for (page = first; page < first + count; page++)
	{
		if (used)
			arena_used[page / 64] |= 1ull << (page % 64);
		else
			arena_used[page / 64] &= ~(1ull << (page % 64));
	}
}

/* the first run of count free pages at or after start, or ARENA_PAGES */
static uint64_t pages_find(uint64_t start, uint64_t count)
{
	uint64_t page = start, run = 0;

	while (page < ARENA_PAGES)
	{
		if (!(page % 64) && arena_used[page / 64] == ~0ull)
		{
			page += 64;
			run = 0;
			continue;
		}
		if (page_used(page))
		{
			run = 0;
		}
		else if (++run == count)
		{
			return page + 1 - count;
		}
		page++;
	}
	return ARENA_PAGES;
}

static int chunks_commit(uint64_t first_page, uint64_t count)
{
	uint64_t chunk = first_page * PAGE / CHUNK;
	uint64_t last = ((first_page + count) * PAGE - 1) / CHUNK;

	for (; chunk <= last; chunk++)
	{
		if (arena_chunk_committed[chunk])
			continue;
		if (commit(ARENA_BASE + chunk * CHUNK, CHUNK, NULL, 0, Perm_Rw) != 0)
			return -1;
		arena_chunk_committed[chunk] = 1;
	}
	return 0;
}

static void *arena_map(uint64_t size, uint32_t permission)
{
	uint64_t count = round_up(size) / PAGE;
	uint64_t first;
	void *result = NULL;

	mutexLock(&memory_lock);
	first = pages_find(arena_hint, count);
	if (first == ARENA_PAGES && arena_hint)
		first = pages_find(0, count);
	if (first != ARENA_PAGES && chunks_commit(first, count) == 0)
	{
		pages_mark(first, count, 1);
		arena_hint = first + count;
		result = (void *)(ARENA_BASE + first * PAGE);
	}
	mutexUnlock(&memory_lock);
	if (result && fresh_pages((uint64_t)result, count * PAGE, permission) != 0)
	{
		host_low_unmap(result, count * PAGE);
		result = NULL;
	}
	return result;
}

void *host_low_map(size_t size, int writable)
{
	return arena_map(size, writable ? Perm_Rw : Perm_R);
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uint64_t)address;
	uint64_t length = round_up(size);

	if (!in_range(start, length, ARENA_BASE, ARENA_END))
		return;
	/* (the pages keep their permission: each change splits the kernel's
	memory blocks, which a process has a limited number of, and musl's
	malloc maps and unmaps often) */
	mutexLock(&memory_lock);
	pages_mark((start - ARENA_BASE) / PAGE, length / PAGE, 0);
	if ((start - ARENA_BASE) / PAGE < arena_hint)
		arena_hint = (start - ARENA_BASE) / PAGE;
	mutexUnlock(&memory_lock);
}

/* 1 if every page of the range is allocated in the arena */
static int arena_owns(uint64_t address, uint64_t length)
{
	uint64_t page;
	int owned = 1;

	if (!in_range(address, length, ARENA_BASE, ARENA_END))
		return 0;
	mutexLock(&memory_lock);
	for (page = (address - ARENA_BASE) / PAGE; page < (address + length - ARENA_BASE) / PAGE; page++)
	{
		if (!page_used(page))
		{
			owned = 0;
			break;
		}
	}
	mutexUnlock(&memory_lock);
	return owned;
}

/* ---------- the guest's memory system calls */

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size);
	uint32_t permission = permission_of(protection);

	(void)offset;
	if (!length)
		return -EINVAL;
	if (!(flags & GUEST_MAP_ANONYMOUS) || fd != -1)
		return -ENODEV;
	if (flags & (GUEST_MAP_FIXED | GUEST_MAP_FIXED_NOREPLACE))
	{
		if (in_range(address, length, CUSTOM_EDITION_BASE, CUSTOM_EDITION_END))
		{
			mutexLock(&memory_lock);
			if (!custom_edition_committed &&
				commit(CUSTOM_EDITION_BASE, CUSTOM_EDITION_END - CUSTOM_EDITION_BASE, NULL, 0, Perm_Rw) == 0)
				custom_edition_committed = 1;
			mutexUnlock(&memory_lock);
			if (!custom_edition_committed)
				return -ENOMEM;
		}
		else if (!in_range(address, length, WINDOW_BASE, WINDOW_END) && !arena_owns(address, length))
		{
			return flags & GUEST_MAP_FIXED_NOREPLACE ? -EEXIST : -EINVAL;
		}
		if (permission == Perm_None)
			return set_permission(address, length, Perm_None) ? -ENOMEM : (long)address;
		return fresh_pages(address, length, permission) ? -ENOMEM : (long)address;
	}
	{
		void *result = arena_map(length, permission);

		return result ? (long)(uintptr_t)result : -ENOMEM;
	}
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size);

	if (in_range(address, length, WINDOW_BASE, WINDOW_END) ||
		(custom_edition_committed && in_range(address, length, CUSTOM_EDITION_BASE, CUSTOM_EDITION_END)))
		return set_permission(address, length, Perm_None) ? -EINVAL : 0;
	if (arena_owns(address, length))
	{
		host_low_unmap((void *)address, length);
		return 0;
	}
	return -EINVAL;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t length = round_up(size);

	if (in_range(address, length, WINDOW_BASE, WINDOW_END) ||
		(custom_edition_committed && in_range(address, length, CUSTOM_EDITION_BASE, CUSTOM_EDITION_END)) ||
		arena_owns(address, length))
		return set_permission(address & ~(PAGE - 1), length, permission_of(protection)) ? -EACCES : 0;
	return -EINVAL;
}
