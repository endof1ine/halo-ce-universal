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
#include <string.h>

void *host_gl_resolve(const char *name)
{
	return (void *)eglGetProcAddress(name);
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
