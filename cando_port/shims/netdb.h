#ifndef CANBOOT_SHIM_NETDB_H
#define CANBOOT_SHIM_NETDB_H
#include <sys/socket.h>

struct addrinfo {
    int                ai_flags;
    int                ai_family;
    int                ai_socktype;
    int                ai_protocol;
    socklen_t          ai_addrlen;
    struct sockaddr   *ai_addr;
    char              *ai_canonname;
    struct addrinfo   *ai_next;
};

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08

#define EAI_FAIL    -4
#define EAI_FAMILY  -6
#define EAI_MEMORY  -10
#define EAI_NONAME  -2
#define EAI_SERVICE -8

#define NI_MAXHOST    1025
#define NI_MAXSERV    32
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02

int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int  getnameinfo(const struct sockaddr *sa, socklen_t salen,
                 char *host, socklen_t hostlen,
                 char *serv, socklen_t servlen, int flags);

#endif
