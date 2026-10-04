/*
PROBE.C

Phase 0 of the Switch port (port/switch/README.md): finds whether this
console lets a homebrew process do what the guest/host design of the
Android port needs, before any of the host is written:

1. map memory at the fixed addresses the game data needs (the Xbox window
   at 0x80000000, the guest image at 0x88000000, the Custom Edition tag
   cache at 0x40440000) and at other addresses below 4 GB;
2. make a page of it executable and run code there;
3. catch a write to a read-only page, make the page writable and resume the
   write (the texture and vertex write tracking of the platform layer), also
   from two threads at once;
4. run a thread with its stack below 4 GB;
5. keep a value in TPIDR_EL0 for each thread;
6. wait on and wake an address below 4 GB (futexes);
7. which OpenGL ES and OpenGL versions and extensions mesa gives.

It writes the results to sdmc:/switch/halo/probe.txt and shows them.
*/

#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WINDOW_BASE 0x80000000ull
#define WINDOW_SIZE 0x08000000ull
#define IMAGE_BASE 0x88000000ull
#define IMAGE_SIZE 0x01000000ull
#define CUSTOM_EDITION_BASE 0x40440000ull
#define CUSTOM_EDITION_SIZE 0x02280000ull
#define CHUNK_SIZE 0x00200000ull
#define LOW_LIMIT 0x100000000ull
#define PAGE 0x1000ull

/* ---------- report */

static char report[64 * 1024];
static size_t report_length;
static int failures;

static void say(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	if (report_length < sizeof(report))
	{
		int length = vsnprintf(report + report_length, sizeof(report) - report_length, format, arguments);

		if (length > 0)
			report_length += (size_t)length;
		if (report_length > sizeof(report))
			report_length = sizeof(report);
	}
	va_end(arguments);
}

static void verdict(const char *test, bool passed, const char *detail)
{
	say("[%s] %s%s%s\n", passed ? "PASS" : "FAIL", test, detail ? ": " : "", detail ? detail : "");
	if (!passed)
		failures++;
}

static void write_report(void)
{
	FILE *file;

	mkdir("sdmc:/switch", 0777);
	mkdir("sdmc:/switch/halo", 0777);
	file = fopen("sdmc:/switch/halo/probe.txt", "wb");
	if (file)
	{
		fwrite(report, 1, report_length, file);
		fclose(file);
	}
}

/* ---------- process and address space */

static u64 info(u32 id)
{
	u64 value = 0;

	if (R_FAILED(svcGetInfo(&value, id, CUR_PROCESS_HANDLE, 0)))
		return 0;
	return value;
}

struct region
{
	const char *name;
	u64 base;
	u64 size;
};

static struct region regions[4];

static void probe_process(void)
{
	static const char *const applet_names[] = {"application", "system applet", "library applet",
		"overlay applet", "system application"};
	AppletType type = appletGetAppletType();
	u64 total = info(InfoType_TotalMemorySize);
	u64 used = info(InfoType_UsedMemorySize);
	int i;

	say("== process\n");
	say("applet type: %d (%s)\n", (int)type,
		type >= 0 && type <= 4 ? applet_names[type] : "none");
	say("memory: total %lu MB, used %lu MB, free %lu MB\n", total >> 20, used >> 20, (total - used) >> 20);
	say("own process handle: %s\n", envGetOwnProcessHandle() != INVALID_HANDLE ? "yes" : "NO");
	say("nso: %s\n", envIsNso() ? "yes" : "no (nro)");
	verdict("title takeover (application)", type == AppletType_Application,
		type == AppletType_Application ? NULL : "start a game while holding R");

	regions[0] = (struct region){"aslr", info(InfoType_AslrRegionAddress), info(InfoType_AslrRegionSize)};
	regions[1] = (struct region){"heap", info(InfoType_HeapRegionAddress), info(InfoType_HeapRegionSize)};
	regions[2] = (struct region){"alias", info(InfoType_AliasRegionAddress), info(InfoType_AliasRegionSize)};
	regions[3] = (struct region){"stack", info(InfoType_StackRegionAddress), info(InfoType_StackRegionSize)};
	for (i = 0; i < 4; i++)
		say("region %-5s %010lx..%010lx (%lu MB)\n", regions[i].name, regions[i].base,
			regions[i].base + regions[i].size, regions[i].size >> 20);
}

/* the heap, alias and stack regions, where svcMapProcessCodeMemory cannot map */
static const struct region *reserved_overlap(u64 base, u64 size)
{
	int i;

	for (i = 1; i < 4; i++)
		if (base < regions[i].base + regions[i].size && regions[i].base < base + size)
			return &regions[i];
	return NULL;
}

static bool range_unmapped(u64 base, u64 size, char *detail, size_t detail_size)
{
	u64 address = base;

	while (address < base + size)
	{
		MemoryInfo memory;
		u32 page_info;

		if (R_FAILED(svcQueryMemory(&memory, &page_info, address)))
		{
			snprintf(detail, detail_size, "query failed at %lx", address);
			return false;
		}
		if (memory.type != MemType_Unmapped)
		{
			snprintf(detail, detail_size, "%lx..%lx is mapped (type %u, perm %u)", memory.addr,
				memory.addr + memory.size, memory.type, memory.perm);
			return false;
		}
		address = memory.addr + memory.size;
	}
	return true;
}

static void probe_low_map(void)
{
	u64 address = 0;
	int entries = 0;

	say("== below 4 GB\n");
	while (address < LOW_LIMIT && entries < 200)
	{
		MemoryInfo memory;
		u32 page_info;

		if (R_FAILED(svcQueryMemory(&memory, &page_info, address)) || !memory.size)
			break;
		say("%010lx..%010lx type %02x perm %u attr %u\n", memory.addr, memory.addr + memory.size,
			memory.type, memory.perm, memory.attr);
		address = memory.addr + memory.size;
		entries++;
	}
}

/* ---------- mapping */

struct mapping
{
	u64 address;
	u64 size;
	void *backing;
	bool mapped;
};

static bool map_fixed(struct mapping *mapping, u64 address, u64 size, char *detail, size_t detail_size)
{
	const struct region *overlap;
	Result result;

	mapping->address = address;
	mapping->size = size;
	mapping->mapped = false;
	overlap = reserved_overlap(address, size);
	if (overlap)
	{
		snprintf(detail, detail_size, "inside the %s region", overlap->name);
		return false;
	}
	if (!range_unmapped(address, size, detail, detail_size))
		return false;
	mapping->backing = aligned_alloc(PAGE, size);
	if (!mapping->backing)
	{
		snprintf(detail, detail_size, "no %lu MB of heap for the backing", size >> 20);
		return false;
	}
	result = svcMapProcessCodeMemory(envGetOwnProcessHandle(), address, (u64)mapping->backing, size);
	if (R_FAILED(result))
	{
		snprintf(detail, detail_size, "svcMapProcessCodeMemory: 0x%x", result);
		free(mapping->backing);
		return false;
	}
	result = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), address, size, Perm_Rw);
	if (R_FAILED(result))
	{
		snprintf(detail, detail_size, "svcSetProcessMemoryPermission(rw): 0x%x", result);
		svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), address, (u64)mapping->backing, size);
		free(mapping->backing);
		return false;
	}
	mapping->mapped = true;
	return true;
}

static bool touch(struct mapping *mapping, char *detail, size_t detail_size)
{
	volatile u32 *first = (volatile u32 *)mapping->address;
	volatile u32 *last = (volatile u32 *)(mapping->address + mapping->size - 4);

	*first = 0x48414c4f;
	*last = 0x4f4c4148;
	if (*first != 0x48414c4f || *last != 0x4f4c4148)
	{
		snprintf(detail, detail_size, "read back differs");
		return false;
	}
	return true;
}

static struct mapping window, image, custom_edition, chunk;

static void probe_fixed_mappings(void)
{
	char detail[160];
	u64 tick;
	bool ok;

	say("== fixed mappings\n");
	tick = armGetSystemTick();
	ok = map_fixed(&window, WINDOW_BASE, WINDOW_SIZE, detail, sizeof(detail)) && touch(&window, detail, sizeof(detail));
	if (ok)
		snprintf(detail, sizeof(detail), "%lu us", armTicksToNs(armGetSystemTick() - tick) / 1000);
	verdict("Xbox window 0x80000000 (128 MB)", ok, detail);

	ok = map_fixed(&image, IMAGE_BASE, IMAGE_SIZE, detail, sizeof(detail)) && touch(&image, detail, sizeof(detail));
	verdict("guest image 0x88000000 (16 MB)", ok, ok ? NULL : detail);

	ok = map_fixed(&custom_edition, CUSTOM_EDITION_BASE, CUSTOM_EDITION_SIZE, detail, sizeof(detail)) &&
		touch(&custom_edition, detail, sizeof(detail));
	verdict("Custom Edition tag cache 0x40440000 (35 MB)", ok, ok ? NULL : detail);
}

/* the largest free gap below 4 GB outside the fixed ranges, for the guest's
other memory (heaps, stacks) */
static void probe_arena(void)
{
	u64 address = 0x10000000ull;
	u64 best = 0, best_size = 0, total = 0;
	char detail[160];
	bool ok;

	say("== low arena\n");
	while (address < LOW_LIMIT)
	{
		MemoryInfo memory;
		u32 page_info;

		if (R_FAILED(svcQueryMemory(&memory, &page_info, address)) || !memory.size)
			break;
		if (memory.type == MemType_Unmapped)
		{
			u64 start = memory.addr < 0x10000000ull ? 0x10000000ull : memory.addr;
			u64 end = memory.addr + memory.size > LOW_LIMIT ? LOW_LIMIT : memory.addr + memory.size;
			int i;

			/* clip the kernel's reserved regions out of the gap */
			for (i = 1; i < 4; i++)
			{
				u64 region_end = regions[i].base + regions[i].size;

				if (regions[i].base <= start && region_end > start)
					start = region_end;
				if (regions[i].base > start && regions[i].base < end)
					end = regions[i].base;
			}
			/* and the fixed ranges */
			if (start < WINDOW_BASE + 0x10000000ull && end > CUSTOM_EDITION_BASE)
			{
				u64 below = CUSTOM_EDITION_BASE - (start < CUSTOM_EDITION_BASE ? start : CUSTOM_EDITION_BASE);
				u64 above = end > WINDOW_BASE + 0x10000000ull ? end - (WINDOW_BASE + 0x10000000ull) : 0;

				if (above >= below)
					start = WINDOW_BASE + 0x10000000ull;
				else
					end = CUSTOM_EDITION_BASE;
			}
			if (end > start)
			{
				total += end - start;
				if (end - start > best_size)
				{
					best = start;
					best_size = end - start;
				}
			}
		}
		address = memory.addr + memory.size;
	}
	say("free below 4 GB (usable): %lu MB; largest gap %lx..%lx (%lu MB)\n", total >> 20, best,
		best + best_size, best_size >> 20);
	verdict("a 512 MB gap for guest heaps and stacks", best_size >= 0x20000000ull, NULL);
	ok = best_size >= CHUNK_SIZE && map_fixed(&chunk, best, CHUNK_SIZE, detail, sizeof(detail)) &&
		touch(&chunk, detail, sizeof(detail));
	verdict("map a 2 MB arena chunk in the gap", ok, ok ? NULL : detail);
}

/* ---------- code */

static void probe_execute(void)
{
	/* mov w0, #0x2a ; ret */
	static const u32 code[] = {0x52800540, 0xd65f03c0};
	u64 page = IMAGE_BASE;
	char detail[96];
	Result result;
	bool ok = false;

	say("== code\n");
	if (!image.mapped)
	{
		verdict("run code in the guest image", false, "no image mapping");
		return;
	}
	memcpy((void *)page, code, sizeof(code));
	armDCacheFlush((void *)page, PAGE);
	result = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), page, PAGE, Perm_Rx);
	if (R_FAILED(result))
	{
		snprintf(detail, sizeof(detail), "svcSetProcessMemoryPermission(rx): 0x%x", result);
	}
	else
	{
		int value;

		armICacheInvalidate((void *)page, PAGE);
		value = ((int (*)(void))page)();
		ok = value == 42;
		snprintf(detail, sizeof(detail), "returned %d", value);
	}
	verdict("run code in the guest image", ok, detail);
}

/* ---------- write tracking */

#define WATCH_PAGES 64
#define WATCH_ROUNDS 200

static atomic_int watch_faults;
static atomic_int watch_unexpected;
static atomic_int watch_in_handler;
static atomic_int watch_overlapped;
static u64 watch_fault_ticks;

u8 __nx_exception_stack[0x8000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *context)
{
	u64 address = context->far.x;

	/* the kernel delivers one user exception of a process at a time: a
	second one here at once would share libnx's single dump and stack */
	if (atomic_fetch_add(&watch_in_handler, 1))
		atomic_fetch_add(&watch_overlapped, 1);
	if (window.mapped && address >= WINDOW_BASE && address < WINDOW_BASE + WINDOW_SIZE)
	{
		u64 tick = armGetSystemTick();

		svcSetProcessMemoryPermission(envGetOwnProcessHandle(), address & ~(PAGE - 1), PAGE, Perm_Rw);
		watch_fault_ticks += armGetSystemTick() - tick;
		atomic_fetch_add(&watch_faults, 1);
	}
	else
	{
		/* skip the instruction, so the probe can report and go on */
		atomic_fetch_add(&watch_unexpected, 1);
		context->pc.x += 4;
	}
	atomic_fetch_sub(&watch_in_handler, 1);
}

static Result protect_pages(u64 base, int count)
{
	return svcSetProcessMemoryPermission(envGetOwnProcessHandle(), base, (u64)count * PAGE, Perm_R);
}

static void write_pages(u64 base, int count, u32 value)
{
	int page;

	for (page = 0; page < count; page++)
		*(volatile u32 *)(base + (u64)page * PAGE + 64) = value;
}

static void watch_thread(void *argument)
{
	u64 base = (u64)argument;
	int round;

	for (round = 0; round < WATCH_ROUNDS; round++)
	{
		protect_pages(base, WATCH_PAGES);
		write_pages(base, WATCH_PAGES, (u32)round);
	}
}

static void probe_write_tracking(void)
{
	char detail[160];
	Thread threads[2];
	Result result;
	int expected, i;
	u64 tick;

	say("== write tracking\n");
	if (!window.mapped)
	{
		verdict("catch writes to read-only pages", false, "no window mapping");
		return;
	}
	result = protect_pages(WINDOW_BASE, WATCH_PAGES);
	if (R_FAILED(result))
	{
		snprintf(detail, sizeof(detail), "svcSetProcessMemoryPermission(r): 0x%x", result);
		verdict("catch writes to read-only pages", false, detail);
		return;
	}
	write_pages(WINDOW_BASE, WATCH_PAGES, 1);
	snprintf(detail, sizeof(detail), "%d of %d writes caught, %d other faults", atomic_load(&watch_faults),
		WATCH_PAGES, atomic_load(&watch_unexpected));
	verdict("catch writes to read-only pages (1 thread)",
		atomic_load(&watch_faults) == WATCH_PAGES && !atomic_load(&watch_unexpected), detail);
	if (atomic_load(&watch_faults) != WATCH_PAGES)
		return;

	atomic_store(&watch_faults, 0);
	watch_fault_ticks = 0;
	tick = armGetSystemTick();
	for (i = 0; i < 2; i++)
	{
		u64 base = WINDOW_BASE + 0x01000000ull * (u64)(i + 1);

		threadCreate(&threads[i], watch_thread, (void *)base, NULL, 0x10000, 0x2c, i);
		threadStart(&threads[i]);
	}
	for (i = 0; i < 2; i++)
	{
		threadWaitForExit(&threads[i]);
		threadClose(&threads[i]);
	}
	expected = 2 * WATCH_ROUNDS * WATCH_PAGES;
	snprintf(detail, sizeof(detail),
		"%d of %d caught, %d overlapping, %lu ns per fault round trip, %lu ns of it in the permission call",
		atomic_load(&watch_faults), expected, atomic_load(&watch_overlapped),
		atomic_load(&watch_faults) ? armTicksToNs(armGetSystemTick() - tick) / (u64)atomic_load(&watch_faults) : 0,
		atomic_load(&watch_faults) ? armTicksToNs(watch_fault_ticks) / (u64)atomic_load(&watch_faults) : 0);
	verdict("catch writes to read-only pages (2 threads on 2 cores)",
		atomic_load(&watch_faults) == expected && !atomic_load(&watch_overlapped), detail);
}

/* ---------- threads */

static u64 thread_stack_pointer;
static u64 thread_tpidr_ok;

static inline u64 read_tpidr(void)
{
	u64 value;

	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
	return value;
}

static inline void write_tpidr(u64 value)
{
	__asm__ volatile("msr tpidr_el0, %0" : : "r"(value));
}

static void stack_thread(void *argument)
{
	u64 sp;

	(void)argument;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	thread_stack_pointer = sp;
}

static void tpidr_thread(void *argument)
{
	u64 mine = 0x1234000000000000ull | (u64)argument;
	int i;

	write_tpidr(mine);
	for (i = 0; i < 2000; i++)
	{
		if (i % 3 == 0)
			svcSleepThread(i % 2 ? 0 : 100000);
		else if (i % 3 == 1)
			svcSleepThread(-1);
		if (read_tpidr() != mine)
			return;
	}
	__atomic_fetch_add(&thread_tpidr_ok, 1, __ATOMIC_SEQ_CST);
}

static void probe_threads(void)
{
	char detail[128];
	Thread thread, threads[3];
	Result result;
	int i;

	say("== threads\n");
	if (!chunk.mapped)
	{
		verdict("thread stack below 4 GB", false, "no arena chunk");
	}
	else
	{
		result = threadCreate(&thread, stack_thread, NULL, (void *)chunk.address, CHUNK_SIZE / 2, 0x2c, -2);
		if (R_SUCCEEDED(result))
			result = threadStart(&thread);
		if (R_SUCCEEDED(result))
		{
			threadWaitForExit(&thread);
			threadClose(&thread);
		}
		snprintf(detail, sizeof(detail), "result 0x%x, sp %lx", result, thread_stack_pointer);
		verdict("thread stack below 4 GB (threadCreate with stack_mem)",
			R_SUCCEEDED(result) && thread_stack_pointer > chunk.address &&
				thread_stack_pointer <= chunk.address + CHUNK_SIZE,
			detail);
	}

	for (i = 0; i < 3; i++)
	{
		threadCreate(&threads[i], tpidr_thread, (void *)(u64)(i + 1), NULL, 0x10000, 0x2c, i);
		threadStart(&threads[i]);
	}
	for (i = 0; i < 3; i++)
	{
		threadWaitForExit(&threads[i]);
		threadClose(&threads[i]);
	}
	snprintf(detail, sizeof(detail), "%lu of 3 threads kept it", thread_tpidr_ok);
	verdict("TPIDR_EL0 kept per thread across sleeps", thread_tpidr_ok == 3, detail);
}

/* ---------- futex */

static volatile u32 *futex_word;
static u64 futex_woken;

static void futex_thread(void *argument)
{
	(void)argument;
	while (__atomic_load_n(futex_word, __ATOMIC_ACQUIRE) == 0)
		svcWaitForAddress((void *)futex_word, ArbitrationType_WaitIfEqual, 0, 1000000000ll);
	futex_woken = 1;
}

static void probe_futex(void)
{
	Thread thread;
	char detail[64];
	Result result;

	say("== futex\n");
	if (!window.mapped)
	{
		verdict("wait and wake on a low address", false, "no window");
		return;
	}
	futex_word = (volatile u32 *)(WINDOW_BASE + 0x04000000ull);
	*futex_word = 0;
	threadCreate(&thread, futex_thread, NULL, NULL, 0x10000, 0x2c, -2);
	threadStart(&thread);
	svcSleepThread(10000000);
	__atomic_store_n(futex_word, 1, __ATOMIC_RELEASE);
	result = svcSignalToAddress((void *)futex_word, SignalType_Signal, 0, 1);
	threadWaitForExit(&thread);
	threadClose(&thread);
	snprintf(detail, sizeof(detail), "signal 0x%x", result);
	verdict("wait and wake on a low address (svcWaitForAddress)", R_SUCCEEDED(result) && futex_woken, detail);
}

/* ---------- graphics */

typedef const unsigned char *(*get_string_function)(unsigned int);
typedef const unsigned char *(*get_stringi_function)(unsigned int, unsigned int);
typedef void (*get_integer_function)(unsigned int, int *);

#define GL_VERSION_ENUM 0x1F02
#define GL_RENDERER_ENUM 0x1F01
#define GL_SHADING_LANGUAGE_VERSION_ENUM 0x8B8C
#define GL_NUM_EXTENSIONS_ENUM 0x821D
#define GL_EXTENSIONS_ENUM 0x1F03
#define GL_NUM_PROGRAM_BINARY_FORMATS_ENUM 0x87FE
#define GL_MAX_TEXTURE_SIZE_ENUM 0x0D33

static const char *const wanted_extensions[] = {
	"GL_EXT_texture_compression_s3tc", "GL_EXT_texture_compression_dxt1", "GL_ANGLE_texture_compression_dxt3",
	"GL_EXT_copy_image", "GL_OES_copy_image", "GL_ARB_copy_image",
	"GL_EXT_draw_elements_base_vertex", "GL_OES_draw_elements_base_vertex", "GL_ARB_draw_elements_base_vertex",
	"GL_ARB_clip_control", "GL_EXT_clip_control", "GL_ARB_query_buffer_object",
	"GL_EXT_buffer_storage", "GL_ARB_buffer_storage", "GL_ARB_shader_atomic_counters",
	"GL_EXT_texture_border_clamp", "GL_OES_texture_border_clamp", "GL_EXT_texture_filter_anisotropic",
	"GL_ARB_texture_filter_anisotropic", "GL_ARB_get_program_binary", "GL_OES_get_program_binary",
	"GL_KHR_parallel_shader_compile", "GL_ARB_parallel_shader_compile", "GL_EXT_texture_format_BGRA8888",
	"GL_ARB_vertex_array_bgra", "GL_ARB_texture_storage", "GL_KHR_debug",
};

static bool has_extension(get_stringi_function get_stringi, int count, const char *name)
{
	int i;

	for (i = 0; i < count; i++)
	{
		const char *extension = (const char *)get_stringi(GL_EXTENSIONS_ENUM, (unsigned)i);

		if (extension && !strcmp(extension, name))
			return true;
	}
	return false;
}

static void probe_context(EGLDisplay display, EGLConfig config, EGLSurface surface, EGLenum api, int major,
	int minor, const char *name)
{
	EGLint attributes[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR, major,
		EGL_CONTEXT_MINOR_VERSION_KHR, minor,
		api == EGL_OPENGL_API ? EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR : EGL_NONE,
		EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
		EGL_NONE,
	};
	get_string_function get_string;
	get_stringi_function get_stringi;
	get_integer_function get_integer;
	EGLContext context;
	int count = 0, binary_formats = 0, max_texture = 0;
	size_t i;

	if (api != EGL_OPENGL_API)
		attributes[4] = EGL_NONE;
	eglBindAPI(api);
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
	{
		char detail[48];

		snprintf(detail, sizeof(detail), "EGL error 0x%x", eglGetError());
		verdict(name, false, detail);
		if (context != EGL_NO_CONTEXT)
			eglDestroyContext(display, context);
		return;
	}
	get_string = (get_string_function)eglGetProcAddress("glGetString");
	get_stringi = (get_stringi_function)eglGetProcAddress("glGetStringi");
	get_integer = (get_integer_function)eglGetProcAddress("glGetIntegerv");
	verdict(name, true, (const char *)get_string(GL_VERSION_ENUM));
	say("  renderer: %s\n  glsl: %s\n", get_string(GL_RENDERER_ENUM), get_string(GL_SHADING_LANGUAGE_VERSION_ENUM));
	get_integer(GL_NUM_EXTENSIONS_ENUM, &count);
	get_integer(GL_NUM_PROGRAM_BINARY_FORMATS_ENUM, &binary_formats);
	get_integer(GL_MAX_TEXTURE_SIZE_ENUM, &max_texture);
	say("  %d extensions, %d program binary formats, max texture %d\n", count, binary_formats, max_texture);
	for (i = 0; i < sizeof(wanted_extensions) / sizeof(*wanted_extensions); i++)
		if (has_extension(get_stringi, count, wanted_extensions[i]))
			say("  + %s\n", wanted_extensions[i]);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
}

static void probe_graphics(void)
{
	static const EGLint config_attributes[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_NONE,
	};
	EGLDisplay display;
	EGLConfig config;
	EGLSurface surface;
	EGLint count = 0;

	say("== graphics\n");
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!display || !eglInitialize(display, NULL, NULL))
	{
		verdict("EGL display", false, NULL);
		return;
	}
	if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count)
	{
		verdict("EGL config (RGBA8, D24S8, ES3 and GL)", false, NULL);
		eglTerminate(display);
		return;
	}
	surface = eglCreateWindowSurface(display, config, nwindowGetDefault(), NULL);
	if (surface == EGL_NO_SURFACE)
	{
		verdict("EGL window surface", false, NULL);
		eglTerminate(display);
		return;
	}
	probe_context(display, config, surface, EGL_OPENGL_ES_API, 3, 2, "OpenGL ES 3.2 context");
	probe_context(display, config, surface, EGL_OPENGL_API, 4, 5, "OpenGL 4.5 core context");
	probe_context(display, config, surface, EGL_OPENGL_API, 4, 3, "OpenGL 4.3 core context");
	eglDestroySurface(display, surface);
	eglTerminate(display);
}

/* ---------- cleanup */

/* the Homebrew Menu's process outlives the probe: give back the fixed
addresses and the heap behind them, so the next run finds them free */
static void unmap(struct mapping *mapping, const char *name)
{
	Result result;

	if (!mapping->mapped)
		return;
	result = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), mapping->address, (u64)mapping->backing,
		mapping->size);
	if (R_SUCCEEDED(result))
		free(mapping->backing);
	else
		say("unmap %s: 0x%x (restart the game before the next run)\n", name, result);
	mapping->mapped = false;
}

/* ---------- main */

int main(int argc, char **argv)
{
	PadState pad;

	(void)argc;
	(void)argv;
	say("Halo Switch probe, %s %s\n", __DATE__, __TIME__);
	probe_process();
	probe_low_map();
	probe_fixed_mappings();
	probe_arena();
	/* the report is saved before each test that can crash the process, so
	a crash leaves the name of the test that was running as its last line */
	write_report();
	probe_execute();
	write_report();
	probe_write_tracking();
	write_report();
	probe_threads();
	write_report();
	probe_futex();
	write_report();
	probe_graphics();
	unmap(&chunk, "arena chunk");
	unmap(&custom_edition, "Custom Edition range");
	unmap(&image, "image");
	unmap(&window, "window");
	say("== %d failure%s\n", failures, failures == 1 ? "" : "s");
	write_report();

	consoleInit(NULL);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	{
		/* the verdicts only; the whole report is in the file */
		char *line = report;

		while (line && *line)
		{
			char *end = strchr(line, '\n');

			if (!strncmp(line, "[", 1) || !strncmp(line, "==", 2) || !strncmp(line, "applet", 6) ||
				!strncmp(line, "memory", 6))
				printf("%.*s\n", end ? (int)(end - line) : (int)strlen(line), line);
			line = end ? end + 1 : NULL;
		}
	}
	printf("\nsdmc:/switch/halo/probe.txt written. Press + to exit.\n");
	while (appletMainLoop())
	{
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
		consoleUpdate(NULL);
	}
	consoleExit(NULL);
	return 0;
}
