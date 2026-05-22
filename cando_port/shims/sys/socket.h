#ifndef CANBOOT_SHIM_SYS_SOCKET_H
#define CANBOOT_SHIM_SYS_SOCKET_H
/* Bare-metal shim. cando's lib/sockutil.h includes us for type
 * forward declarations; the actual socket calls live in .c files that
 * are NOT compiled into the canboot kernel. */

#include <stddef.h>
#include <stdint.h>

typedef unsigned short sa_family_t;
typedef uint32_t       socklen_t;

struct sockaddr {
    sa_family_t   sa_family;
    char          sa_data[14];
};
struct sockaddr_storage {
    sa_family_t   ss_family;
    unsigned char ss_pad[126];
};

#define AF_UNSPEC 0
#define AF_INET   2
#define AF_INET6 10
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#endif
