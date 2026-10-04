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
#include <SDL3/SDL_video.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

/* the screen's size; docked, the system scales it to the television */
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define WINDOW_HANDLE 1
#define CONTEXT_HANDLE 1
#define EVENT_QUEUE_SIZE 64

static char last_error[256];
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
static EGLContext context = EGL_NO_CONTEXT;
static int requested_major = 3, requested_minor = 2;

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
	nwindowSetDimensions(nwindowGetDefault(), SCREEN_WIDTH, SCREEN_HEIGHT);
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
	*width = SCREEN_WIDTH;
	*height = SCREEN_HEIGHT;
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
	return 1;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	EGLint attributes[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR, requested_major,
		EGL_CONTEXT_MINOR_VERSION_KHR, requested_minor,
		EGL_NONE,
	};

	if (window != WINDOW_HANDLE || surface == EGL_NO_SURFACE)
		return 0;
	if (context == EGL_NO_CONTEXT)
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
	if (context == EGL_NO_CONTEXT)
	{
		set_error("eglCreateContext failed");
		return 0;
	}
	host_logf(HOST_LOG_INFO, "OpenGL ES %d.%d context", requested_major, requested_minor);
	return CONTEXT_HANDLE;
}

int host_sdl_gl_make_current(uint32_t window, uint32_t gl_context)
{
	if (window != WINDOW_HANDLE || gl_context != CONTEXT_HANDLE)
		return eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ? 1 : 0;
	return eglMakeCurrent(display, surface, surface, context) ? 1 : 0;
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return eglSwapInterval(display, interval) ? 1 : 0;
}

int host_sdl_gl_swap_window(uint32_t window)
{
	(void)window;
	return eglSwapBuffers(display, surface) ? 1 : 0;
}

/* ---------- the clipboard (internet play's invite links): the program's own */

static char clipboard[1024];

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
