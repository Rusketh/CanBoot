#ifndef CANBOOT_TFTP_H
#define CANBOOT_TFTP_H

#include <stdint.h>
#include "lwip/ip_addr.h"

/*
 * Minimal read-only TFTP client (RFC 1350) over lwIP's raw UDP API. Synchronous
 * - drives the lwIP pump until the transfer completes or times out, matching
 * the single-threaded style of the cando net bindings. Used by the init.cdo
 * loader to fetch the boot script from a PXE/TFTP server.
 *
 * Reads `filename` from `server` (port 69) in octet mode into `out` (capacity
 * `out_cap`). On success returns 0 and sets *out_len to the byte count. Returns
 * non-zero on timeout, server error, or if the file would exceed out_cap.
 */
int canboot_tftp_get(const ip_addr_t *server, const char *filename,
                     char *out, uint32_t out_cap, uint32_t *out_len);

#endif /* CANBOOT_TFTP_H */
