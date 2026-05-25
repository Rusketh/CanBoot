#ifndef CANBOOT_RESOLVER_H
#define CANBOOT_RESOLVER_H

#include <stdint.h>
#include "lwip/ip_addr.h"

/* Synchronous DNS resolution over lwIP's UDP resolver. Drives the net
 * pump + lwIP timers until the query completes or `timeout_ms` elapses.
 * Returns 0 and fills *out on success, -1 on failure. NO_SYS-safe: the
 * caller is the only flow touching lwIP. */
int canboot_dns_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms);

#endif /* CANBOOT_RESOLVER_H */
