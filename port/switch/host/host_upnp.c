/*
HOST_UPNP.C

Internet play's UPnP on the Switch is the desktop's and Android's
(port/linux/src/posix_upnp.c, with port/third_party/miniupnpc), built into
the host. These are what miniupnpc needs that libnx has not
(compat/upnp_compat.h): interface names, which only IPv6 addresses' scopes
use, and the Switch's network has none.
*/

#include "compat/upnp_compat.h"

#include <stddef.h>

unsigned int if_nametoindex(const char *name)
{
	(void)name;
	return 0;
}

char *if_indextoname(unsigned int index, char *name)
{
	(void)index;
	(void)name;
	return NULL;
}
