#ifndef CANBOOT_SHIM_ARPA_INET_H
#define CANBOOT_SHIM_ARPA_INET_H
#include <sys/socket.h>
#include <netinet/in.h>
/* Bare-metal shim. picolibc's <arpa/inet.h> ships only the byte-order
 * macros; cando doesn't need anything more from the .h files we let
 * through, but we keep this guard so includes don't double up. */
#endif
