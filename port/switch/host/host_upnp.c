/*
HOST_UPNP.C

Internet play's UPnP (port/linux/src/posix.h): the Switch port has none
yet, so a game it hosts on the internet needs its router's port forwarded
by hand, or another player hosting. port/linux/src/posix_upnp.c with
miniupnpc could serve it later.
*/

#include "posix.h"

#include <stdio.h>

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	(void)external_address;
	(void)external_port;
	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "the Switch port has no UPnP");
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}
