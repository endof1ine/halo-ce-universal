/*
HOST_BINK.C

The guest's movie imports (guest_host.h): the Android port has no movie
decoder, and its guest does not call them (port/linux/src/bink_null.c).
*/

#include "host.h"

uint32_t host_bink_open(const char *path, uint32_t *description)
{
	(void)path;
	(void)description;
	return 0;
}

int host_bink_decode(uint32_t movie)
{
	(void)movie;
	return 0;
}

void host_bink_copy(uint32_t movie, void *destination, int pitch, uint32_t height)
{
	(void)movie;
	(void)destination;
	(void)pitch;
	(void)height;
}

void host_bink_close(uint32_t movie)
{
	(void)movie;
}
