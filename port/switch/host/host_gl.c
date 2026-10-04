/*
HOST_GL.C

OpenGL ES for the guest (switch-mesa, nouveau). Its generated entry points
(guest_gl.c) import hostgl_<function>, resolved here through EGL; the
arguments already have host types by then. These helpers serve the
renderer's needs that cross the 32-bit boundary (port/android/host/host_gl.c
is the Android host's).
*/

#include "host.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <string.h>
#include <switch.h>

/* ---------- timing: the GL work that makes frames late

Shader compiles and links (mesa compiles on the CPU, the first time an
effect is drawn) and texture uploads are timed here, by wrapping the
functions the guest imports; host_sdl.c logs the totals with the frame
rates. */

struct gl_timing host_gl_timing;

static void (GL_APIENTRY *real_compile_shader)(GLuint);
static void (GL_APIENTRY *real_link_program)(GLuint);
static void (GL_APIENTRY *real_tex_image_2d)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
	const void *);
static void (GL_APIENTRY *real_tex_sub_image_2d)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
	const void *);
static void (GL_APIENTRY *real_compressed_tex_image_2d)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
	const void *);

static uint64_t now_ns(void)
{
	return armTicksToNs(armGetSystemTick());
}

static void GL_APIENTRY timed_compile_shader(GLuint shader)
{
	uint64_t start = now_ns();

	real_compile_shader(shader);
	host_gl_timing.shader_ns += now_ns() - start;
	host_gl_timing.shaders++;
}

static void GL_APIENTRY timed_link_program(GLuint program)
{
	uint64_t start = now_ns();

	real_link_program(program);
	host_gl_timing.shader_ns += now_ns() - start;
	host_gl_timing.programs++;
}

static void GL_APIENTRY timed_tex_image_2d(GLenum target, GLint level, GLint internal_format, GLsizei width,
	GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	uint64_t start = now_ns();

	real_tex_image_2d(target, level, internal_format, width, height, border, format, type, pixels);
	host_gl_timing.texture_ns += now_ns() - start;
	host_gl_timing.textures++;
}

static void GL_APIENTRY timed_tex_sub_image_2d(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
	GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	uint64_t start = now_ns();

	real_tex_sub_image_2d(target, level, x, y, width, height, format, type, pixels);
	host_gl_timing.texture_ns += now_ns() - start;
	host_gl_timing.textures++;
}

static void GL_APIENTRY timed_compressed_tex_image_2d(GLenum target, GLint level, GLenum internal_format,
	GLsizei width, GLsizei height, GLint border, GLsizei size, const void *data)
{
	uint64_t start = now_ns();

	real_compressed_tex_image_2d(target, level, internal_format, width, height, border, size, data);
	host_gl_timing.texture_ns += now_ns() - start;
	host_gl_timing.textures++;
}

void *host_gl_resolve(const char *name)
{
	void *function = (void *)eglGetProcAddress(name);

	if (!function)
		return NULL;
#define TIMED(gl_name, real, timed) \
	if (!strcmp(name, gl_name)) \
	{ \
		real = function; \
		return (void *)timed; \
	}
	TIMED("glCompileShader", real_compile_shader, timed_compile_shader)
	TIMED("glLinkProgram", real_link_program, timed_link_program)
	TIMED("glTexImage2D", real_tex_image_2d, timed_tex_image_2d)
	TIMED("glTexSubImage2D", real_tex_sub_image_2d, timed_tex_sub_image_2d)
	TIMED("glCompressedTexImage2D", real_compressed_tex_image_2d, timed_compressed_tex_image_2d)
#undef TIMED
	return function;
}

/* switch-mesa exports no GL functions, only eglGetProcAddress: the ones used
here, resolved at the first use (with a context current) */
static struct
{
	const GLubyte *(GL_APIENTRY *GetString)(GLenum);
	const GLubyte *(GL_APIENTRY *GetStringi)(GLenum, GLuint);
	void (GL_APIENTRY *GetIntegerv)(GLenum, GLint *);
	void (GL_APIENTRY *BindBuffer)(GLenum, GLuint);
	void *(GL_APIENTRY *MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
	GLboolean (GL_APIENTRY *UnmapBuffer)(GLenum);
	void (GL_APIENTRY *BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
	GLsync (GL_APIENTRY *FenceSync)(GLenum, GLbitfield);
	void (GL_APIENTRY *DeleteSync)(GLsync);
	GLenum (GL_APIENTRY *ClientWaitSync)(GLsync, GLbitfield, GLuint64);
	GLenum (GL_APIENTRY *GetError)(void);
	void (GL_APIENTRY *BufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield);
} gl;

static void gl_load(void)
{
	if (gl.GetString)
		return;
	gl.GetStringi = host_gl_resolve("glGetStringi");
	gl.GetIntegerv = host_gl_resolve("glGetIntegerv");
	gl.BindBuffer = host_gl_resolve("glBindBuffer");
	gl.MapBufferRange = host_gl_resolve("glMapBufferRange");
	gl.UnmapBuffer = host_gl_resolve("glUnmapBuffer");
	gl.BufferSubData = host_gl_resolve("glBufferSubData");
	gl.FenceSync = host_gl_resolve("glFenceSync");
	gl.DeleteSync = host_gl_resolve("glDeleteSync");
	gl.ClientWaitSync = host_gl_resolve("glClientWaitSync");
	gl.GetError = host_gl_resolve("glGetError");
	gl.BufferStorage = host_gl_resolve("glBufferStorageEXT");
	gl.GetString = host_gl_resolve("glGetString");
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text;

	gl_load();
	text = index >= 0 ? gl.GetStringi(name, (GLuint)index) : gl.GetString(name);
	if (!size)
		return;
	buffer[0] = 0;
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	gl_load();
	gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)gl.GetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* one 32-bit word of a buffer object (the visibility tests' counters of
d3d8_gl.c): ES has no glGetBufferSubData, and the mapping it offers instead
is a host pointer */
uint32_t host_gl_read_buffer_word(uint32_t buffer, uint32_t offset)
{
	uint32_t value = 0;
	GLint previous = 0;
	const void *mapping;

	gl_load();
	gl.GetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &previous);
	gl.BindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
	mapping = gl.MapBufferRange(GL_ATOMIC_COUNTER_BUFFER, offset, sizeof(value), GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(&value, mapping, sizeof(value));
		gl.UnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);
	}
	gl.BindBuffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)previous);
	return value;
}

/* The renderer streams each frame's vertices and indices into the next of a
ring of buffers (d3d8_gl.c), and writes a buffer again only once the GPU
has passed the fence of the frame that last used it. */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	gl_load();
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		gl.DeleteSync(frame_fences[slot]);
	frame_fences[slot] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(uint32_t slot)
{
	gl_load();
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	gl.ClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	gl.DeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* writes data into the buffer bound to target, unsynchronized: the ring
slot it writes is one no queued draw reads (host_gl_wait_frame). Without
GL_MAP_INVALIDATE_RANGE_BIT, which makes drivers discard and so stall
(port/android/host/host_gl.c) */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping;

	gl_load();
	mapping = gl.MapBufferRange(target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	if (!mapping)
	{
		gl.BufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	gl.UnmapBuffer(target);
}

/* ---------- the stream ring, mapped for good

With EXT_buffer_storage the ring's buffers are mapped once, persistently and
coherently: a write is then a copy, not a mapping, a copy and an unmapping
in the driver each time. The ring's fences still keep a buffer from being
written while queued draws read it. */

#define PERSISTENT_BUFFERS 8
#define PERSISTENT_FLAGS (GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT)

static struct
{
	GLuint buffer;
	uint8_t *mapping;
	uint32_t size;
} persistent[PERSISTENT_BUFFERS];

/* gives the buffer bound to target (a new one, with no storage yet) size
bytes, mapped for good; 0 when it cannot, and then the buffer may be
unusable: the guest makes another */
int host_gl_buffer_persist(uint32_t target, uint32_t size)
{
	GLint buffer = 0;
	void *mapping;
	int slot;

	gl_load();
	if (!gl.BufferStorage || !host_gl_has_extension("GL_EXT_buffer_storage"))
		return 0;
	for (slot = 0; slot < PERSISTENT_BUFFERS && persistent[slot].buffer; slot++)
		;
	gl.GetIntegerv(target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING,
		&buffer);
	if (slot == PERSISTENT_BUFFERS || !buffer)
		return 0;
	gl.BufferStorage(target, size, NULL, PERSISTENT_FLAGS);
	mapping = gl.GetError() == GL_NO_ERROR ? gl.MapBufferRange(target, 0, size, PERSISTENT_FLAGS) : NULL;
	if (!mapping)
	{
		while (gl.GetError() != GL_NO_ERROR)
			;
		host_logf(HOST_LOG_WARN, "cannot map stream buffer %d for good", buffer);
		return 0;
	}
	persistent[slot].buffer = (GLuint)buffer;
	persistent[slot].mapping = mapping;
	persistent[slot].size = size;
	return 1;
}

/* writes data into a ring buffer, by its name: a copy into its mapping, or
(one host_gl_buffer_persist could not map) as host_gl_buffer_write does */
void host_gl_buffer_write_to(uint32_t buffer, uint32_t offset, uint32_t size, const void *data)
{
	int slot;

	for (slot = 0; slot < PERSISTENT_BUFFERS && persistent[slot].buffer; slot++)
	{
		if (persistent[slot].buffer == buffer)
		{
			if (offset <= persistent[slot].size && size <= persistent[slot].size - offset)
			{
				memcpy(persistent[slot].mapping + offset, data, size);
				/* (the Tegra's GPU does not see the CPU's caches: the copy
				written through to memory) */
				armDCacheFlush(persistent[slot].mapping + offset, size);
			}
			return;
		}
	}
	gl_load();
	gl.BindBuffer(GL_COPY_WRITE_BUFFER, buffer);
	host_gl_buffer_write(GL_COPY_WRITE_BUFFER, offset, size, data);
}
