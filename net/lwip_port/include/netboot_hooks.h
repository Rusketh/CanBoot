#ifndef CANBOOT_NETBOOT_HOOKS_H
#define CANBOOT_NETBOOT_HOOKS_H

/*
 * lwIP hook surface for PXE/netboot. Wired via LWIP_HOOK_FILENAME +
 * LWIP_HOOK_DHCP_PARSE_OPTION in lwipopts.h so the DHCP client can learn the
 * TFTP server address from the OFFER/ACK (BOOTP siaddr / option 66 / option
 * 150) without patching vendored lwIP source. The diskless boot path then
 * pulls /init.cdo from that server over TFTP (see net/tftp.c).
 */

#include "lwip/arch.h"
#include "lwip/ip_addr.h"

struct dhcp_msg;
struct pbuf;

/* Called per unhandled DHCP option from dhcp_parse_reply (the macro in
 * lwipopts.h drops the netif/dhcp/state args we don't use). */
void canboot_dhcp_parse_option(struct dhcp_msg *msg, u8_t msg_type,
                               u8_t option, u8_t len,
                               struct pbuf *p, u16_t offset);

/* Best TFTP server learned from DHCP so far. Returns 1 and fills *out when an
 * address is known, 0 otherwise (no netboot server advertised). */
int canboot_netboot_tftp_server(ip_addr_t *out);

#endif /* CANBOOT_NETBOOT_HOOKS_H */
