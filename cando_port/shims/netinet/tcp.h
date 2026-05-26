#ifndef CANBOOT_SHIM_NETINET_TCP_H
#define CANBOOT_SHIM_NETINET_TCP_H
/* TCP-level socket option names. TCP_NODELAY / IPPROTO_TCP already live
 * in <sys/socket.h>; this header exists so cando's lib/sockutil.c
 * #include <netinet/tcp.h> resolves on the freestanding build. */
#include <sys/socket.h>
#endif
