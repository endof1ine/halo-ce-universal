/*
HOST.H

Internals of the Switch port's host: the libnx program that maps the guest
image (the game, built as ILP32 AArch64 code) at its fixed address and
serves its calls. See port/switch/README.md for the design, and
port/android/include/halo_android_abi.h for the contract with the guest,
which the Android host also serves.
*/

#ifndef __HALO_SWITCH_HOST_H
#define __HALO_SWITCH_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

/* the game's folders on the SD card (the default device, so plain POSIX
paths) */
#define HOST_DATA_ROOT "/switch/halo"
#define HOST_SAVE_ROOT "/switch/halo/save"

/* ---------- logging (host_main.c): to host.log and the nxlink host */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
/* Android's log priorities, which the guest passes to host_log */
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6

/* logs, shows the message to the player (the error applet) and ends the
process */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);
/* a newlib errno value as the guest's (Linux) one */
int host_linux_errno(int error);
/* a true/false setting of config.toml, false (or missing) when it is not
there */
int host_config_boolean(const char *key);
int host_config_boolean_default(const char *key, int missing);
/* a number setting of config.toml (read from the file at each call) */
double host_config_real(const char *key, double missing);
/* the sampling profiler of the main thread (host_profile.c) */
void host_profile_start(void);

/* ---------- memory below 4 GB (host_memory.c)

Guest code addresses everything through 32-bit pointers. The host commits
the fixed ranges the game data needs (the Xbox window at 0x80000000, the
guest image above it) and an arena above them for the rest (the guest's
malloc, thread stacks), all with svcMapProcessCodeMemory from the heap: the
only way a homebrew program maps memory at an address of its choosing. */

/* checks the address space and commits the window; 0 on success, else a
reason for the player */
const char *host_memory_initialize(void);
/* commits [address, address + size) for the image's code (executable,
filled from data first: code memory cannot be written once mapped) or data;
0 on success */
int host_memory_map_code(uint32_t address, const void *data, uint32_t size);
int host_memory_map_data(uint32_t address, uint32_t size);
/* pages of the arena; NULL when it is full */
void *host_low_map(size_t size, int writable);
void host_low_unmap(void *address, size_t size);
/* the guest's mmap/munmap/mprotect (host_syscall.c): Linux arguments and
results */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);

/* ---------- write tracking (host_watch.c) */

void host_watch_start(void);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; 0 on success */
int host_load_image(const void *elf, size_t size);
/* the host function for an import name, or NULL (generated table) */
void *host_resolve_import(const char *name);
void *host_gl_resolve(const char *name);
/* the GL work timed so far (host_gl.c) */
struct gl_timing
{
	uint64_t shader_ns, texture_ns;
	uint32_t shaders, programs, textures;
};
extern struct gl_timing host_gl_timing;

/* ---------- threads (host_thread.c) */

/* what a host thread is for: its core and priority */
enum host_thread_role
{
	/* the guest's threads but its main one: preemptively shared */
	_host_thread_guest,
	/* the audio mixer: above the guest's */
	_host_thread_audio,
	/* write tracking: below everything */
	_host_thread_background,
};

/* calls the guest function at address with up to four 32-bit arguments
on this thread, which must be on a guest stack (host_thread_run_on_guest_stack
or host_native_thread_create), giving it a guest thread first if it has none */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) on a stack in the arena,
which is freed after it returns; 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size,
	enum host_thread_role role);
/* runs function(argument) on this thread with a stack of size in the arena */
void host_thread_run_on_guest_stack(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on this thread; does not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));

/* ---------- SDL services (host_sdl.c, host_input.c, host_audio.c) */

/* the applet's messages, once a frame from the event pump */
void host_sdl_applet_update(void);
void host_input_initialize(void);
/* polls the controllers; queues an SDL gamepad-added event for each new one */
void host_input_update(void);
void host_input_stop_rumble(void);
void host_audio_pause(int paused);
/* SDL3 events made by the host (host_sdl.c) */
void host_sdl_queue_gamepad_added(uint32_t id);
/* relative mouse motion: the platform layer's mouse look (gyro aiming) */
void host_sdl_queue_mouse_motion(float x, float y);

#endif
