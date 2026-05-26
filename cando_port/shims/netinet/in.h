#ifndef CANBOOT_SHIM_NETINET_IN_H
#define CANBOOT_SHIM_NETINET_IN_H
#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

#define INET_ADDRSTRLEN   16
#define INET6_ADDRSTRLEN  46

struct in_addr  { in_addr_t s_addr; };
struct in6_addr { unsigned char s6_addr[16]; };

struct sockaddr_in {
    sa_family_t     sin_family;
    in_port_t       sin_port;
    struct in_addr  sin_addr;
    unsigned char   sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

#define INADDR_ANY        ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK   ((in_addr_t)0x7f000001)

/* Network byte order = big-endian; canboot targets are little-endian. */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#endif
