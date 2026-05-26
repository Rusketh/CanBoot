#ifndef CANBOOT_SHIM_SYS_SOCKET_H
#define CANBOOT_SHIM_SYS_SOCKET_H
/*
 * Bare-metal BSD-socket surface for CanDo's lib/sockutil.c (and the
 * socket / secure_socket / http / https libraries layered on it). Backed
 * by the lwIP-raw socket layer in cando_port/net_posix/sockets.c.
 */

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

#define AF_UNSPEC   0
#define AF_INET     2
#define AF_INET6   10
#define PF_INET     AF_INET
#define PF_UNSPEC   AF_UNSPEC

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define SOL_SOCKET     0xFFFF
#define SO_REUSEADDR   0x0004
#define SO_KEEPALIVE   0x0008
#define SO_SNDBUF      0x1001
#define SO_RCVBUF      0x1002
#define SO_SNDTIMEO    0x1005
#define SO_RCVTIMEO    0x1006
#define SO_ERROR       0x1007

#define IPPROTO_IP    0
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
#define TCP_NODELAY   0x01

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define MSG_NOSIGNAL 0
#define MSG_PEEK     0x01

int  socket(int domain, int type, int protocol);
int  connect(int fd, const struct sockaddr *addr, socklen_t len);
int  bind(int fd, const struct sockaddr *addr, socklen_t len);
int  listen(int fd, int backlog);
int  accept(int fd, struct sockaddr *addr, socklen_t *len);
long send(int fd, const void *buf, size_t len, int flags);
long recv(int fd, void *buf, size_t len, int flags);
int  setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int  getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int  getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int  getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int  shutdown(int fd, int how);

#endif
