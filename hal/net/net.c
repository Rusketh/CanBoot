/*
 * Generic NIC layer: probe registered drivers in order, bind the first
 * one that brings up a link, and dispatch the hal_net_* surface to it.
 * Drivers live in hal/net/<name>.c and export a `struct canboot_nic`.
 */

#include <stddef.h>

#include "hal/net.h"

/* MMIO drivers: available on every arch. */
extern const struct canboot_nic canboot_nic_virtio;
extern const struct canboot_nic canboot_nic_e1000;
extern const struct canboot_nic canboot_nic_e1000e;

/* Port-I/O drivers: x86_64 only (legacy PIO BARs). */
#if defined(__x86_64__)
extern const struct canboot_nic canboot_nic_rtl8139;
extern const struct canboot_nic canboot_nic_pcnet;
#endif

static const struct canboot_nic *const g_drivers[] = {
    &canboot_nic_virtio,
    &canboot_nic_e1000,
    &canboot_nic_e1000e,
#if defined(__x86_64__)
    &canboot_nic_rtl8139,
    &canboot_nic_pcnet,
#endif
};

#define NUM_DRIVERS (sizeof(g_drivers) / sizeof(g_drivers[0]))

static const struct canboot_nic *g_active;

bool canboot_net_init(void) {
    if (g_active) return true;
    for (unsigned i = 0; i < NUM_DRIVERS; i++) {
        if (g_drivers[i]->init()) {
            g_active = g_drivers[i];
            return true;
        }
    }
    return false;
}

bool canboot_net_present(void) { return g_active != NULL; }

const char *canboot_net_active_name(void) {
    return g_active ? g_active->name : "none";
}

void hal_net_pump(void) {
    if (g_active) g_active->pump();
}

const uint8_t *hal_net_mac(void) {
    static const uint8_t zero[CANBOOT_NET_MAC_LEN] = { 0 };
    return g_active ? g_active->mac() : zero;
}

struct netif *canboot_net_netif(void) {
    return g_active ? g_active->netif() : NULL;
}
