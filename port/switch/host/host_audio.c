/*
HOST_AUDIO.C

The guest's SDL3 audio stream (dsound_sdl.c mixes into one: 48 kHz,
stereo, floats, pulled through a callback), on audout: 16-bit stereo at
48 kHz, the system's own format, in buffers of 512 frames (10.7 ms) of
which BUFFER_COUNT are queued.

A thread of the host's (core 2, above the guest's other threads) waits for
audout to finish a buffer, asks the guest's callback for its frames on a
stack below 4 GB (host_thread.c), converts them and queues the buffer
again.
*/

#include "host.h"

#include <SDL3/SDL_audio.h>
#include <malloc.h>
#include <string.h>
#include <switch.h>

#define FRAMES 512
#define CHANNELS 2
#define BUFFER_COUNT 3
#define BUFFER_BYTES 0x1000 /* audout's alignment; holds the 2 KB of a buffer */
#define STREAM_HANDLE 1
/* the guest's frames converted but not queued yet */
#define STAGING_FRAMES (FRAMES * 8)

static AudioOutBuffer buffers[BUFFER_COUNT];
static int16_t staging[STAGING_FRAMES * CHANNELS];
static uint32_t staged_frames;
static uint32_t callback, userdata;
static int opened, running, paused;
static Mutex audio_lock;
static CondVar audio_condition;

static int16_t sample_of(float value)
{
	if (value >= 1.0f)
		return 32767;
	if (value <= -1.0f)
		return -32768;
	return (int16_t)(value * 32767.0f);
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	const float *samples = data;
	uint32_t frames, index;

	if (stream != STREAM_HANDLE || length < 0)
		return 0;
	frames = (uint32_t)length / (sizeof(float) * CHANNELS);
	if (frames > STAGING_FRAMES - staged_frames)
		frames = STAGING_FRAMES - staged_frames;
	for (index = 0; index < frames * CHANNELS; index++)
		staging[staged_frames * CHANNELS + index] = sample_of(samples[index]);
	staged_frames += frames;
	return 1;
}

/* fills a buffer: the staged frames, then the guest's callback for the
rest, then silence */
static void fill(AudioOutBuffer *buffer)
{
	int16_t *out = buffer->buffer;

	if (staged_frames < FRAMES && !paused)
	{
		int wanted = (int)((FRAMES - staged_frames) * sizeof(float) * CHANNELS);

		host_call_guest(callback, userdata, STREAM_HANDLE, (uint32_t)wanted, (uint32_t)wanted);
	}
	if (staged_frames >= FRAMES)
	{
		memcpy(out, staging, FRAMES * CHANNELS * sizeof(int16_t));
		staged_frames -= FRAMES;
		memmove(staging, staging + FRAMES * CHANNELS, staged_frames * CHANNELS * sizeof(int16_t));
	}
	else
	{
		memcpy(out, staging, staged_frames * CHANNELS * sizeof(int16_t));
		memset(out + staged_frames * CHANNELS, 0, (FRAMES - staged_frames) * CHANNELS * sizeof(int16_t));
		staged_frames = 0;
	}
	buffer->data_size = FRAMES * CHANNELS * sizeof(int16_t);
}

static void *audio_thread(void *unused)
{
	int index;

	(void)unused;
	mutexLock(&audio_lock);
	while (!running)
		condvarWait(&audio_condition, &audio_lock);
	mutexUnlock(&audio_lock);
	for (index = 0; index < BUFFER_COUNT; index++)
	{
		memset(buffers[index].buffer, 0, BUFFER_BYTES);
		buffers[index].data_size = FRAMES * CHANNELS * sizeof(int16_t);
		audoutAppendAudioOutBuffer(&buffers[index]);
	}
	for (;;)
	{
		AudioOutBuffer *released = NULL;
		u32 count = 0;

		if (R_FAILED(audoutWaitPlayFinish(&released, &count, UINT64_MAX)) || !released)
			continue;
		fill(released);
		audoutAppendAudioOutBuffer(released);
	}
	return NULL;
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t guest_callback,
	uint32_t guest_userdata)
{
	const SDL_AudioSpec *format = spec;
	int index;
	Result result;

	(void)device;
	if (opened)
		return 0;
	if (!guest_callback || !format || format->format != SDL_AUDIO_F32 || format->channels != CHANNELS ||
		format->freq != 48000)
	{
		host_logf(HOST_LOG_ERROR, "audio stream format %d/%d/%d is not 48 kHz stereo floats",
			format ? (int)format->format : 0, format ? format->channels : 0, format ? format->freq : 0);
		return 0;
	}
	mutexInit(&audio_lock);
	condvarInit(&audio_condition);
	result = audoutInitialize();
	if (R_SUCCEEDED(result))
		result = audoutStartAudioOut();
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "audout: 0x%x", result);
		return 0;
	}
	for (index = 0; index < BUFFER_COUNT; index++)
	{
		buffers[index].buffer = memalign(BUFFER_BYTES, BUFFER_BYTES);
		buffers[index].buffer_size = BUFFER_BYTES;
		if (!buffers[index].buffer)
			return 0;
	}
	callback = guest_callback;
	userdata = guest_userdata;
	if (host_native_thread_create(audio_thread, NULL, 256 * 1024, _host_thread_audio) != 0)
		host_fatal("Cannot start the audio thread.");
	opened = 1;
	host_logf(HOST_LOG_INFO, "audio: %d buffers of %d frames at %u Hz", BUFFER_COUNT, FRAMES, audoutGetSampleRate());
	return STREAM_HANDLE;
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	if (stream != STREAM_HANDLE || !opened)
		return 0;
	mutexLock(&audio_lock);
	running = 1;
	condvarWakeAll(&audio_condition);
	mutexUnlock(&audio_lock);
	return 1;
}

/* while the game is in the background its mixer is not called (its
threads are suspended anyway) and the buffers play silence */
void host_audio_pause(int pause)
{
	paused = pause;
}
