/*
HOST_SDL.C

The SDL3 functions the guest calls (port/android/guest/runtime/guest_sdl.c),
on libnx: the guest's platform layer is written for SDL3, which has no
Switch port, and needs little of it: a window with an OpenGL ES context
(EGL on the default display window), events, gamepads (host_input.c) and an
audio stream (host_audio.c).

Events are SDL3's own structures, made here from the applet's messages:
SDL_Event has the same layout for both ABIs for every event without a
pointer, which are all the guest reads.
*/

#include "host.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

/* the screen's size: the console's in handheld mode, 1080 lines docked (the
surface is made again when the console is docked or taken out) */
#define HANDHELD_WIDTH 1280
#define HANDHELD_HEIGHT 720
#define DOCKED_WIDTH 1920
#define DOCKED_HEIGHT 1080
#define WINDOW_HANDLE 1
#define CONTEXT_HANDLE 1
#define EVENT_QUEUE_SIZE 64

static char last_error[256];
/* internet play's invite links (the clipboard, below) */
static char clipboard[1024];
static uint64_t start_tick;

static void set_error(const char *text)
{
	snprintf(last_error, sizeof(last_error), "%s", text);
}

/* ---------- events */

static SDL_Event event_queue[EVENT_QUEUE_SIZE];
static unsigned event_head, event_count;
static Mutex event_lock;

static void queue_event(const SDL_Event *event)
{
	mutexLock(&event_lock);
	if (event_count < EVENT_QUEUE_SIZE)
	{
		event_queue[(event_head + event_count) % EVENT_QUEUE_SIZE] = *event;
		event_count++;
	}
	mutexUnlock(&event_lock);
}

static void queue_simple(Uint32 type)
{
	SDL_Event event;

	memset(&event, 0, sizeof(event));
	event.type = type;
	event.common.timestamp = armTicksToNs(armGetSystemTick());
	if (type >= SDL_EVENT_WINDOW_FIRST && type <= SDL_EVENT_WINDOW_LAST)
		event.window.windowID = WINDOW_HANDLE;
	queue_event(&event);
}

void host_sdl_queue_gamepad_added(uint32_t id)
{
	SDL_Event event;

	memset(&event, 0, sizeof(event));
	event.type = SDL_EVENT_GAMEPAD_ADDED;
	event.common.timestamp = armTicksToNs(armGetSystemTick());
	event.gdevice.which = (SDL_JoystickID)id;
	queue_event(&event);
}

void host_sdl_queue_mouse_motion(float x, float y)
{
	SDL_Event event;

	memset(&event, 0, sizeof(event));
	event.type = SDL_EVENT_MOUSE_MOTION;
	event.common.timestamp = armTicksToNs(armGetSystemTick());
	event.motion.windowID = WINDOW_HANDLE;
	event.motion.xrel = x;
	event.motion.yrel = y;
	queue_event(&event);
}

static void surface_resize(int docked);

/* the applet's messages: HOME and sleep take the focus, the system asks
the program to close */
void host_sdl_applet_update(void)
{
	static AppletFocusState focus = AppletFocusState_InFocus;
	static AppletOperationMode mode = AppletOperationMode_Handheld;
	AppletFocusState now;

	if (!appletMainLoop())
	{
		host_logf(HOST_LOG_INFO, "the system asks the game to close");
		queue_simple(SDL_EVENT_QUIT);
	}
	now = appletGetFocusState();
	if (now != focus)
	{
		focus = now;
		host_logf(HOST_LOG_INFO, "focus %s", now == AppletFocusState_InFocus ? "gained" : "lost");
		if (now == AppletFocusState_InFocus)
		{
			host_audio_pause(0);
			queue_simple(SDL_EVENT_WINDOW_FOCUS_GAINED);
		}
		else
		{
			host_audio_pause(1);
			host_input_stop_rumble();
			queue_simple(SDL_EVENT_WINDOW_FOCUS_LOST);
		}
	}
	if (appletGetOperationMode() != mode)
	{
		mode = appletGetOperationMode();
		host_logf(HOST_LOG_INFO, "%s", mode == AppletOperationMode_Console ? "docked" : "handheld");
		surface_resize(mode == AppletOperationMode_Console);
	}
}

int host_sdl_poll_event(void *event)
{
	int found = 0;

	mutexLock(&event_lock);
	if (!event_count)
	{
		mutexUnlock(&event_lock);
		/* a new round of polling: the applet and the controllers */
		host_sdl_applet_update();
		host_input_update();
		mutexLock(&event_lock);
	}
	if (event_count)
	{
		memcpy(event, &event_queue[event_head], sizeof(SDL_Event));
		event_head = (event_head + 1) % EVENT_QUEUE_SIZE;
		event_count--;
		found = 1;
	}
	mutexUnlock(&event_lock);
	return found;
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	(void)flags;
	mutexInit(&event_lock);
	if (!start_tick)
		start_tick = armGetSystemTick();
	return 1;
}

int host_sdl_set_hint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return 1;
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	if (size)
		snprintf(buffer, size, "%s", last_error);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)(armTicksToNs(armGetSystemTick() - start_tick) / 1000000ull);
}

int64_t host_sdl_thread_id(void)
{
	u64 id = 0;

	svcGetThreadId(&id, CUR_THREAD_HANDLE);
	return (int64_t)id;
}

/* ---------- video: EGL on the default window */

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLConfig config;
static int surface_width = HANDHELD_WIDTH, surface_height = HANDHELD_HEIGHT, swap_interval = 1;
/* [CONTEXT_HANDLE] the game's; the others share its objects (its shader
compiling threads, d3d8_gl.c), and are made current without a surface */
#define CONTEXTS 4
static EGLContext contexts[CONTEXTS];
static int requested_major = 3, requested_minor = 2, requested_share;

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	static const EGLint attributes[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		/* the renderer draws into framebuffers of its own and blits */
		EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
		EGL_NONE,
	};
	EGLint count = 0;

	(void)title;
	(void)width;
	(void)height;
	(void)flags;
	if (surface != EGL_NO_SURFACE)
		return WINDOW_HANDLE;
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL))
	{
		set_error("eglInitialize failed");
		return 0;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count)
	{
		set_error("no EGL configuration");
		return 0;
	}
	if (appletGetOperationMode() == AppletOperationMode_Console)
	{
		surface_width = DOCKED_WIDTH;
		surface_height = DOCKED_HEIGHT;
	}
	nwindowSetDimensions(nwindowGetDefault(), (u32)surface_width, (u32)surface_height);
	surface = eglCreateWindowSurface(display, config, nwindowGetDefault(), NULL);
	if (surface == EGL_NO_SURFACE)
	{
		set_error("eglCreateWindowSurface failed");
		return 0;
	}
	return WINDOW_HANDLE;
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	(void)window;
	*width = surface_width;
	*height = surface_height;
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	(void)window;
	(void)enabled;
	return 1;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	if (attribute == SDL_GL_CONTEXT_MAJOR_VERSION)
		requested_major = value;
	else if (attribute == SDL_GL_CONTEXT_MINOR_VERSION)
		requested_minor = value;
	else if (attribute == SDL_GL_SHARE_WITH_CURRENT_CONTEXT)
		requested_share = value;
	return 1;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	EGLint attributes[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR, requested_major,
		EGL_CONTEXT_MINOR_VERSION_KHR, requested_minor,
		EGL_NONE,
	};

	uint32_t handle;

	if (window != WINDOW_HANDLE || surface == EGL_NO_SURFACE)
		return 0;
	if (!contexts[CONTEXT_HANDLE] || !requested_share)
	{
		if (!contexts[CONTEXT_HANDLE])
			contexts[CONTEXT_HANDLE] = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
		if (!contexts[CONTEXT_HANDLE])
		{
			set_error("eglCreateContext failed");
			return 0;
		}
		host_logf(HOST_LOG_INFO, "OpenGL ES %d.%d context", requested_major, requested_minor);
		return CONTEXT_HANDLE;
	}
	for (handle = CONTEXT_HANDLE + 1; handle < CONTEXTS; handle++)
	{
		if (!contexts[handle])
			break;
	}
	if (handle == CONTEXTS)
	{
		set_error("no more contexts");
		return 0;
	}
	contexts[handle] = eglCreateContext(display, config, contexts[CONTEXT_HANDLE], attributes);
	if (!contexts[handle])
	{
		set_error("eglCreateContext (shared) failed");
		host_logf(HOST_LOG_WARN, "cannot create a shared OpenGL context: 0x%x", eglGetError());
		return 0;
	}
	return handle;
}

int host_sdl_gl_make_current(uint32_t window, uint32_t gl_context)
{
	if (window != WINDOW_HANDLE || !gl_context || gl_context >= CONTEXTS || !contexts[gl_context])
		return eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ? 1 : 0;
	if (gl_context == CONTEXT_HANDLE)
		return eglMakeCurrent(display, surface, surface, contexts[gl_context]) ? 1 : 0;
	/* (the window's surface is the game's thread's: EGL_KHR_surfaceless_context) */
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, contexts[gl_context]))
	{
		host_logf(HOST_LOG_WARN, "cannot make a shared OpenGL context current: 0x%x", eglGetError());
		return 0;
	}
	return 1;
}

int host_sdl_gl_set_swap_interval(int interval)
{
	swap_interval = interval;
	return eglSwapInterval(display, interval) ? 1 : 0;
}

/* docked or taken out: the window's surface again at the screen's size,
between frames on the game's thread (its context's), which the game's
next frame draws to (d3d8_gl.c reads the size every frame) */
static void surface_resize(int docked)
{
	int width = docked ? DOCKED_WIDTH : HANDHELD_WIDTH, height = docked ? DOCKED_HEIGHT : HANDHELD_HEIGHT;

	if (surface == EGL_NO_SURFACE || !contexts[CONTEXT_HANDLE] || width == surface_width)
		return;
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(display, surface);
	nwindowSetDimensions(nwindowGetDefault(), (u32)width, (u32)height);
	surface = eglCreateWindowSurface(display, config, nwindowGetDefault(), NULL);
	if (surface == EGL_NO_SURFACE)
		host_fatal("Cannot draw to the screen after docking (EGL 0x%x).", eglGetError());
	eglMakeCurrent(display, surface, surface, contexts[CONTEXT_HANDLE]);
	eglSwapInterval(display, swap_interval);
	surface_width = width;
	surface_height = height;
	host_logf(HOST_LOG_INFO, "screen %dx%d", width, height);
}

/* frame times, logged every 10 seconds: the average rate, the slowest
frame and the frames over 33 ms (below 30 per second) */
static void frame_statistics(void)
{
	static uint64_t period_start, last_frame, slowest;
	static unsigned frames, slow_frames;
	uint64_t now = armTicksToNs(armGetSystemTick());

	if (last_frame)
	{
		uint64_t duration = now - last_frame;

		if (duration > slowest)
			slowest = duration;
		if (duration > 33333333ull)
			slow_frames++;
	}
	last_frame = now;
	if (!period_start)
		period_start = now;
	frames++;
	if (now - period_start >= 10000000000ull)
	{
		host_logf(HOST_LOG_INFO, "frames: %.1f per second, slowest %.1f ms, %u over 33 ms; "
			"%u shaders and %u programs in %.1f ms, %u texture uploads in %.1f ms",
			frames * 1e9 / (double)(now - period_start), slowest / 1e6, slow_frames,
			host_gl_timing.shaders, host_gl_timing.programs, host_gl_timing.shader_ns / 1e6,
			host_gl_timing.textures, host_gl_timing.texture_ns / 1e6);
		memset(&host_gl_timing, 0, sizeof(host_gl_timing));
		period_start = now;
		frames = slow_frames = 0;
		slowest = 0;
	}
}

int host_sdl_gl_swap_window(uint32_t window)
{
	(void)window;
	frame_statistics();
	return eglSwapBuffers(display, surface) ? 1 : 0;
}

/* ---------- key names (the settings' key bindings): SDL's, for the keys a
Switch has no keyboard for anyway, but those of letters, digits and a few */

static const struct
{
	int scancode;
	const char *name;
} key_names[] = {
	{ SDL_SCANCODE_RETURN, "Return" }, { SDL_SCANCODE_ESCAPE, "Escape" },
	{ SDL_SCANCODE_BACKSPACE, "Backspace" }, { SDL_SCANCODE_TAB, "Tab" }, { SDL_SCANCODE_SPACE, "Space" },
	{ SDL_SCANCODE_LSHIFT, "Left Shift" }, { SDL_SCANCODE_LCTRL, "Left Ctrl" },
	{ SDL_SCANCODE_LALT, "Left Alt" }, { SDL_SCANCODE_UP, "Up" }, { SDL_SCANCODE_DOWN, "Down" },
	{ SDL_SCANCODE_LEFT, "Left" }, { SDL_SCANCODE_RIGHT, "Right" },
};

void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	unsigned index;

	if (!size)
		return;
	buffer[0] = 0;
	if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
		snprintf(buffer, size, "%c", 'A' + (scancode - SDL_SCANCODE_A));
	else if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_0)
		snprintf(buffer, size, "%c", scancode == SDL_SCANCODE_0 ? '0' : '1' + (scancode - SDL_SCANCODE_1));
	for (index = 0; index < sizeof(key_names) / sizeof(*key_names); index++)
	{
		if (key_names[index].scancode == scancode)
			snprintf(buffer, size, "%s", key_names[index].name);
	}
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	unsigned index;

	if (!name || !*name)
		return SDL_SCANCODE_UNKNOWN;
	if (!name[1] && ((name[0] | 0x20) >= 'a' && (name[0] | 0x20) <= 'z'))
		return SDL_SCANCODE_A + ((name[0] | 0x20) - 'a');
	if (!name[1] && name[0] >= '0' && name[0] <= '9')
		return name[0] == '0' ? SDL_SCANCODE_0 : SDL_SCANCODE_1 + (name[0] - '1');
	for (index = 0; index < sizeof(key_names) / sizeof(*key_names); index++)
	{
		if (!strcasecmp(key_names[index].name, name))
			return key_names[index].scancode;
	}
	return SDL_SCANCODE_UNKNOWN;
}

/* ---------- the clipboard (internet play's invite links): the program's own */

int host_sdl_set_clipboard_text(const char *text)
{
	snprintf(clipboard, sizeof(clipboard), "%s", text ? text : "");
	return 1;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	if (size)
		snprintf(buffer, size, "%s", clipboard);
}

/* ---------- notices for the player: logged */

/* the system's keyboard with the clipboard's text: the invite link of a game
the console hosts, to read out, or another's to join. A different link goes
to the clipboard, and the game, told it came to the front, joins it
(network.join_from_clipboard, sdl_platform.c) */
void host_sdl_invite_keyboard(void)
{
	SwkbdConfig keyboard;
	char text[sizeof(clipboard)] = "";
	Result result;

	if (R_FAILED(swkbdCreate(&keyboard, 0)))
		return;
	swkbdConfigMakePresetDefault(&keyboard);
	swkbdConfigSetHeaderText(&keyboard, clipboard[0] ? "Your game's invite link, or another to join" :
		"An invite link to join");
	swkbdConfigSetGuideText(&keyboard, "halo://join/...");
	swkbdConfigSetInitialText(&keyboard, clipboard);
	swkbdConfigSetOkButtonText(&keyboard, "Join");
	swkbdConfigSetStringLenMax(&keyboard, sizeof(clipboard) - 1);
	result = swkbdShow(&keyboard, text, sizeof(text));
	swkbdClose(&keyboard);
	if (R_FAILED(result) || !text[0] || !strcmp(text, clipboard))
		return;
	snprintf(clipboard, sizeof(clipboard), "%s", text);
	host_logf(HOST_LOG_INFO, "invite link entered");
	queue_simple(SDL_EVENT_WINDOW_FOCUS_GAINED);
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "notice: %s", message);
	return 1;
}

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	(void)flags;
	host_logf(HOST_LOG_WARN, "message: %s: %s", title ? title : "", message ? message : "");
	return 1;
}
