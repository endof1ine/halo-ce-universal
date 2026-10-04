/*
SYS/UN.H

The local socket address miniupnpc's MiniSSDPd client names
(port/third_party/miniupnpc/src/minissdpc.c), in libnx's BSD layout. The
Switch has no local sockets: socket(AF_UNIX) fails, and miniupnpc then
searches the network itself.
*/

#ifndef HALO_SWITCH_SYS_UN_H
#define HALO_SWITCH_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un
{
	unsigned char sun_len;
	sa_family_t sun_family;
	char sun_path[104];
};

#endif
