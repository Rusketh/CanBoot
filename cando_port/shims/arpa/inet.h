#ifndef CANBOOT_SHIM_ARPA_INET_H
#define CANBOOT_SHIM_ARPA_INET_H
#include <sys/socket.h>
#include <netinet/in.h>

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);

#endif
