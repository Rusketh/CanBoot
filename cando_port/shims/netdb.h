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

#endif
