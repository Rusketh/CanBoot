#ifndef CANBOOT_HAL_NET_H
#define CANBOOT_HAL_NET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * HAL net interface. virtio-net only today; the same surface fans out
 * to e1000/rtl8169 + real hardware NICs later.
 *
 * Discovery + bring-up live in canboot_virtio_net_init(). The driver
 * registers an lwIP netif on success; callers use lwIP raw APIs for
 * UDP/TCP/DHCP and call hal_net_pump() from their main loop to drain
 * the virtqueue used ring and dispatch frames into lwIP.
 */

#define CANBOOT_NET_MAC_LEN 6

bool       canboot_virtio_net_init(void);
bool       canboot_virtio_net_present(void);
void       hal_net_pump(void);
const uint8_t *hal_net_mac(void);

#endif /* CANBOOT_HAL_NET_H */
