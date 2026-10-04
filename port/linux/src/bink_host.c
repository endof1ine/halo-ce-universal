/*
BINK_HOST.C

The Bink video SDK entry points bink_playback.c uses, on the Switch: the
host decodes the movie with FFmpeg (port/switch/host/host_bink.c) and plays
its sound itself; this paces the frames by the movie's rate and copies them
into the game's texture. The other ports skip the movies (bink_null.c).

The prototypes match the declarations in bink_playback.c; the RAD SDK's
RADEXPLINK is __stdcall.
*/

#ifdef HALO_SWITCH

#include "platform.h"
#include "port_config.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the host's movies (guest_host.h) */
unsigned int host_bink_open(const char *path, unsigned int *description);
int host_bink_decode(unsigned int movie);
void host_bink_copy(unsigned int movie, void *destination, int pitch, unsigned int height);
void host_bink_close(unsigned int movie);

typedef void *(__stdcall *rad_memory_allocate_proc)(unsigned long size);
typedef void (__stdcall *rad_memory_free_proc)(void *memory);
typedef void *(__stdcall *bink_sound_system_open_proc)(unsigned long param);

/* the SDK's handle: the fields bink_playback.c reads first, as its BINK */
struct bink
{
	unsigned long Width;
	unsigned long Height;
	unsigned long Frames;
	unsigned long FrameNum;
	unsigned long LastFrameNum;

	unsigned int movie;
	unsigned long rate, rate_divisor;
	unsigned long long start_ns;
	BOOL decoded;
};
typedef struct bink *HBINK;

/* bink_playback.c's BINKSUMMARY and BINKREALTIME, whose size it checks:
only the first fields are filled */
#define BINK_SUMMARY_SIZE 0x7c
#define BINK_REALTIME_SIZE 0x38

static unsigned long long now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000000000ULL + (unsigned long long)now.tv_nsec;
}

void __stdcall RADSetMemory(rad_memory_allocate_proc allocate, rad_memory_free_proc release)
{
	(void)allocate;
	(void)release;
}

void *__stdcall BinkOpenDirectSound(unsigned long param)
{
	(void)param;
	return NULL;
}

/* (the host plays the sound: always as asked) */
long __stdcall BinkSetSoundSystem(bink_sound_system_open_proc open, unsigned long param)
{
	(void)open;
	(void)param;
	return 1;
}

void __stdcall BinkSetIOSize(unsigned long io_size)
{
	(void)io_size;
}

HBINK __stdcall BinkOpen(const char *name, unsigned long flags)
{
	char path[1024];
	unsigned int description[5];
	struct bink *bink;
	unsigned int movie;

	(void)flags;
	/* (display.movies off: as if there were none) */
	if (!config_boolean("display.movies"))
		return NULL;
	platform_translate_path(name, path, sizeof(path));
	movie = host_bink_open(path, description);
	if (!movie)
	{
		platform_log("movie %s: none (its folder is next to maps/)", name);
		return NULL;
	}
	/* (bink_playback.c ends a movie at its next to last frame: one of no
	known length would never end) */
	if (!description[2])
	{
		platform_log("movie %s: no frame count, skipped", name);
		host_bink_close(movie);
		return NULL;
	}
	bink = calloc(1, sizeof(*bink));
	if (!bink)
	{
		host_bink_close(movie);
		return NULL;
	}
	bink->movie = movie;
	bink->Width = description[0];
	bink->Height = description[1];
	bink->Frames = description[2];
	bink->rate = description[3] ? description[3] : 30;
	bink->rate_divisor = description[4] ? description[4] : 1;
	bink->FrameNum = 1;
	bink->start_ns = now_ns();
	platform_log("movie %s: %lux%lu, %lu frames at %.2f a second", name, bink->Width, bink->Height, bink->Frames,
		(double)bink->rate / (double)bink->rate_divisor);
	return bink;
}

void __stdcall BinkClose(HBINK bink)
{
	if (!bink)
		return;
	host_bink_close(bink->movie);
	free(bink);
}

/* nonzero while it is not yet time for the next frame */
long __stdcall BinkWait(HBINK bink)
{
	unsigned long long due;

	if (!bink)
		return 0;
	due = bink->start_ns + (unsigned long long)(bink->FrameNum - 1) * 1000000000ULL * bink->rate_divisor / bink->rate;
	return now_ns() < due;
}

long __stdcall BinkDoFrame(HBINK bink)
{
	if (!bink)
		return 0;
	bink->decoded = host_bink_decode(bink->movie) != 0;
	return 0;
}

void __stdcall BinkNextFrame(HBINK bink)
{
	if (!bink)
		return;
	bink->LastFrameNum = bink->FrameNum;
	/* (past the last frame the movie ends: bink_playback.c stops at the
	next to last) */
	if (bink->decoded && bink->FrameNum < bink->Frames)
		bink->FrameNum++;
	else
		bink->FrameNum = bink->Frames ? bink->Frames - 1 : 0;
}

long __stdcall BinkCopyToBuffer(HBINK bink, void *destination, long destination_pitch,
	unsigned long destination_height, unsigned long destination_x, unsigned long destination_y,
	unsigned long flags)
{
	(void)destination_x;
	(void)destination_y;
	(void)flags;
	if (!bink || !destination)
		return 1;
	host_bink_copy(bink->movie, destination, (int)destination_pitch, (unsigned int)destination_height);
	return 0;
}

void __stdcall BinkGetSummary(HBINK bink, void *summary)
{
	unsigned long *fields = summary;

	memset(summary, 0, BINK_SUMMARY_SIZE);
	if (!bink)
		return;
	fields[0] = bink->Width;
	fields[1] = bink->Height;
	fields[3] = bink->rate;
	fields[4] = bink->rate_divisor;
	fields[5] = bink->rate;
	fields[6] = bink->rate_divisor;
	fields[8] = bink->Frames;
	fields[9] = bink->FrameNum;
}

void __stdcall BinkGetRealtime(HBINK bink, void *realtime, unsigned long frame_count)
{
	unsigned long *fields = realtime;

	(void)frame_count;
	memset(realtime, 0, BINK_REALTIME_SIZE);
	if (!bink)
		return;
	fields[0] = bink->FrameNum;
	fields[1] = bink->rate;
	fields[2] = bink->rate_divisor;
	fields[3] = bink->Frames;
}

#endif
