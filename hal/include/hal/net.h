#ifndef CANBOOT_HAL_NET_H
#define CANBOOT_HAL_NET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * HAL net interface.
 *
 * A small pluggable NIC layer: each driver exposes a `struct canboot_nic`
 * (probe/bring-up, cooperative pump, MAC, lwIP netif). canboot_net_init()
 * walks the registered drivers in order and the first that brings up a
 * link wins; everything else talks to the active NIC through the generic
 * hal_net and canboot_net surface below. Drivers register an lwIP netif on
 * success; callers use lwIP raw APIs for UDP/TCP/DHCP and call
 * hal_net_pump() from their main loop to drain the NIC and dispatch frames
 * into lwIP.
 *
 * Drivers today: virtio-net (MMIO, all arches), Intel e1000 / e1000e
 * (MMIO, all arches), RTL8139 and AMD PCnet (port I/O, x86_64 only).
 */

#define CANBOOT_NET_MAC_LEN 6

struct netif; /* lwIP forward decl */

struct canboot_nic {
    const char     *name;
    bool          (*init)(void);    /* probe + bring up netif; false if absent */
    void          (*pump)(void);    /* drain RX/TX (cooperative)               */
    const uint8_t *(*mac)(void);    /* 6-byte hardware address                 */
    struct netif  *(*netif)(void);  /* the registered lwIP netif               */
};

/* Probe every registered driver; returns true once one brings up a link.
 * Safe to call once during bring-up. */
bool        canboot_net_init(void);
bool        canboot_net_present(void);
const char *canboot_net_active_name(void);

/* Generic surface, dispatched to the active driver. */
void           hal_net_pump(void);
const uint8_t *hal_net_mac(void);
struct netif  *canboot_net_netif(void);

#endif /* CANBOOT_HAL_NET_H */
