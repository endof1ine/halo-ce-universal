/*
UPNP_COMPAT.H

Included before each of miniupnpc's sources on the Switch: what libnx's
net/if.h declares only in a comment (host_upnp.c defines them).
*/

#ifndef HALO_SWITCH_UPNP_COMPAT_H
#define HALO_SWITCH_UPNP_COMPAT_H

unsigned int if_nametoindex(const char *name);
char *if_indextoname(unsigned int index, char *name);

#endif
