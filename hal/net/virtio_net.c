/*
 * Modern virtio-net driver wired into lwIP's NO_SYS netif. One RX
 * queue (qidx=0) is pre-stuffed with NUM_BUFS receive buffers; one TX
 * queue (qidx=1) pulls from a small round-robin pool of TX buffers.
 * No offloads negotiated, so virtio_net_hdr_v1 is just a 12-byte zero
 * prefix on every frame.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hal/net.h"
#include "hal/virtio.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#define VIRTIO_NET_PCI_ID_MODERN       0x1041u  /* non-transitional */
#define VIRTIO_NET_PCI_ID_TRANSITIONAL 0x1000u  /* legacy + modern caps */

#define VIRTIO_NET_F_MAC     5u
#define VIRTIO_NET_F_STATUS  16u
#define VIRTIO_F_VERSION_1   32u

#define RX_QIDX   0u
#define TX_QIDX   1u
#define NUM_BUFS  16u
#define FRAME_MAX 1600u

struct __attribute__((packed)) virtio_net_hdr_v1 {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

#define HDR_LEN   ((uint32_t)sizeof(struct virtio_net_hdr_v1))
#define BUF_TOTAL (HDR_LEN + FRAME_MAX)

static struct canboot_virtio_dev g_dev;
static struct canboot_virtq g_rxq, g_txq;

static __attribute__((aligned(16)))
struct canboot_virtq_desc  rx_desc[CANBOOT_VIRTQ_SIZE];
static __attribute__((aligned(2)))
struct canboot_virtq_avail rx_avail;
static __attribute__((aligned(4)))
struct canboot_virtq_used  rx_used;

static __attribute__((aligned(16)))
struct canboot_virtq_desc  tx_desc[CANBOOT_VIRTQ_SIZE];
static __attribute__((aligned(2)))
struct canboot_virtq_avail tx_avail;
static __attribute__((aligned(4)))
struct canboot_virtq_used  tx_used;

static __attribute__((aligned(16))) uint8_t rx_bufs[NUM_BUFS][BUF_TOTAL];
static __attribute__((aligned(16))) uint8_t tx_bufs[NUM_BUFS][BUF_TOTAL];

static bool     g_present;
static uint8_t  g_mac[CANBOOT_NET_MAC_LEN];
static struct   netif g_netif;
static uint16_t g_tx_next;

static uint32_t g_stat_tx_calls;
static uint32_t g_stat_tx_kicked;
static uint32_t g_stat_rx_done;
static uint32_t g_stat_tx_done;

uint32_t canboot_virtio_net_stat_tx_calls(void)  { return g_stat_tx_calls; }
uint32_t canboot_virtio_net_stat_tx_kicked(void) { return g_stat_tx_kicked; }
uint32_t canboot_virtio_net_stat_rx_done(void)   { return g_stat_rx_done; }
uint32_t canboot_virtio_net_stat_tx_done(void)   { return g_stat_tx_done; }

static err_t virtio_net_linkout(struct netif *nif, struct pbuf *p) {
    (void)nif;
    if (p == NULL) return ERR_VAL;
    g_stat_tx_calls++;
    uint16_t slot = g_tx_next;
    g_tx_next = (uint16_t)((g_tx_next + 1u) % NUM_BUFS);

    uint8_t *buf = tx_bufs[slot];
    memset(buf, 0, HDR_LEN);
    uint16_t copied = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if ((uint32_t)copied + q->len > FRAME_MAX) return ERR_BUF;
        memcpy(buf + HDR_LEN + copied, q->payload, q->len);
        copied = (uint16_t)(copied + q->len);
    }
    uint32_t total = HDR_LEN + copied;

    canboot_virtq_publish_readable(&g_txq, slot, buf, total);
    canboot_virtq_kick(&g_txq, TX_QIDX);
    g_stat_tx_kicked++;
    return ERR_OK;
}

static err_t virtio_net_netif_init(struct netif *nif) {
    nif->name[0] = 'v';
    nif->name[1] = 'n';
    nif->output     = etharp_output;
    nif->linkoutput = virtio_net_linkout;
    nif->mtu        = 1500;
    nif->hwaddr_len = CANBOOT_NET_MAC_LEN;
    memcpy(nif->hwaddr, g_mac, CANBOOT_NET_MAC_LEN);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void dispatch_rx(uint32_t id, uint32_t len) {
    if (id >= NUM_BUFS) return;
    if (len <= HDR_LEN) return;
    uint16_t frame_len = (uint16_t)(len - HDR_LEN);
    if (frame_len > FRAME_MAX) return;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, frame_len, PBUF_RAM);
    if (!p) return;
    pbuf_take(p, rx_bufs[id] + HDR_LEN, frame_len);

    if (g_netif.input(p, &g_netif) != ERR_OK) {
        pbuf_free(p);
    }
}

void hal_net_pump(void) {
    if (!g_present) return;

    uint16_t rx_completed = canboot_virtq_used_advance(&g_rxq);
    for (uint16_t i = 0; i < rx_completed; i++) {
        uint16_t slot = (uint16_t)((g_rxq.last_used_idx + i) % g_rxq.size);
        uint32_t id   = g_rxq.used->ring[slot].id;
        uint32_t len  = g_rxq.used->ring[slot].len;
        dispatch_rx(id, len);
        if (id < g_rxq.size) {
            canboot_virtq_publish_writable(&g_rxq, (uint16_t)id,
                                           rx_bufs[id % NUM_BUFS], BUF_TOTAL);
        }
    }
    if (rx_completed) {
        g_stat_rx_done += rx_completed;
        g_rxq.last_used_idx = (uint16_t)(g_rxq.last_used_idx + rx_completed);
        canboot_virtq_kick(&g_rxq, RX_QIDX);
    }

    /* Advance TX used idx; we don't need per-buffer cleanup since
     * virtio_net_linkout cycles through tx_bufs[]. */
    uint16_t tx_completed = canboot_virtq_used_advance(&g_txq);
    if (tx_completed) {
        g_stat_tx_done += tx_completed;
        g_txq.last_used_idx = (uint16_t)(g_txq.last_used_idx + tx_completed);
    }
}

bool canboot_virtio_net_present(void) { return g_present; }
const uint8_t *hal_net_mac(void) { return g_mac; }
struct netif *canboot_virtio_net_netif(void) { return &g_netif; }

bool canboot_virtio_net_init(void) {
    if (!canboot_virtio_find(VIRTIO_NET_PCI_ID_MODERN, &g_dev)) {
        if (!canboot_virtio_find(VIRTIO_NET_PCI_ID_TRANSITIONAL, &g_dev)) {
            return false;
        }
    }

    uint64_t want = (1ull << VIRTIO_F_VERSION_1)
                  | (1ull << VIRTIO_NET_F_MAC)
                  | (1ull << VIRTIO_NET_F_STATUS);
    if (!canboot_virtio_negotiate(&g_dev, want)) return false;

    if (g_dev.device_cfg) {
        for (int i = 0; i < CANBOOT_NET_MAC_LEN; i++) {
            g_mac[i] = g_dev.device_cfg[i];
        }
    }

    if (!canboot_virtio_queue_setup(&g_dev, RX_QIDX, &g_rxq,
                                    rx_desc, &rx_avail, &rx_used)) {
        return false;
    }
    if (!canboot_virtio_queue_setup(&g_dev, TX_QIDX, &g_txq,
                                    tx_desc, &tx_avail, &tx_used)) {
        return false;
    }

    uint16_t to_publish = NUM_BUFS;
    if (to_publish > g_rxq.size) to_publish = g_rxq.size;
    for (uint16_t i = 0; i < to_publish; i++) {
        canboot_virtq_publish_writable(&g_rxq, i, rx_bufs[i], BUF_TOTAL);
    }

    if (!canboot_virtio_run(&g_dev)) return false;
    canboot_virtq_kick(&g_rxq, RX_QIDX);

    ip4_addr_t any_addr = { 0 };
    netif_add(&g_netif, &any_addr, &any_addr, &any_addr,
              NULL, virtio_net_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    g_present = true;
    return true;
}
