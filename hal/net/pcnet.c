/*
 * AMD Am79C970A "PCnet-PCI II/III" driver wired into lwIP's NO_SYS netif
 * (PCI 1022:2000/2001 — VirtualBox's "PCnet" NICs, QEMU `-device pcnet`).
 * 32-bit DWIO access, SWSTYLE 2 init block + descriptor rings, MAC from
 * APROM. Polled from hal_net_pump(). x86_64 only (legacy PIO).
 *
 * Probe-gated: if no PCnet device is present, init() returns false and
 * nothing here is touched.
 */

#if defined(__x86_64__)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hal/net.h"
#include "hal/pci.h"

#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#define PCNET_VENDOR 0x1022u

/* DWIO (32-bit) register offsets. */
#define REG_APROM 0x00u
#define REG_RDP   0x10u
#define REG_RAP   0x14u
#define REG_RESET 0x18u

#define CSR0_INIT 0x0001u
#define CSR0_STRT 0x0002u
#define CSR0_STOP 0x0004u
#define CSR0_TDMD 0x0008u
#define CSR0_IDON 0x0100u

#define DESC_OWN 0x80000000u
#define DESC_ERR 0x40000000u
#define DESC_STP 0x02000000u
#define DESC_ENP 0x01000000u
#define DESC_ONES 0x0000F000u

#define NRXD     8u
#define NTXD     8u
#define LOG2_8   3u
#define BUF_SIZE 2048u

struct __attribute__((packed, aligned(16))) pcnet_desc {
    uint32_t base;    /* buffer phys                          */
    uint32_t flags;   /* OWN/STP/ENP/ONES/bcnt (2's-comp len) */
    uint32_t msg_len; /* RX: received byte count (bits 11:0)  */
    uint32_t resv;
};

struct __attribute__((packed)) pcnet_init32 {
    uint16_t mode;
    uint8_t  rlen;        /* log2(NRXD) << 4 */
    uint8_t  tlen;        /* log2(NTXD) << 4 */
    uint8_t  phys[6];
    uint16_t reserved;
    uint8_t  ladrf[8];
    uint32_t rdra;        /* rx ring phys */
    uint32_t tdra;        /* tx ring phys */
};

static __attribute__((aligned(16))) struct pcnet_desc g_rxd[NRXD];
static __attribute__((aligned(16))) struct pcnet_desc g_txd[NTXD];
static __attribute__((aligned(16))) uint8_t g_rxbuf[NRXD][BUF_SIZE];
static __attribute__((aligned(16))) uint8_t g_txbuf[NTXD][BUF_SIZE];
static __attribute__((aligned(16))) struct pcnet_init32 g_initblk;

static uint16_t g_io;
static bool     g_present;
static uint8_t  g_mac[CANBOOT_NET_MAC_LEN];
static struct   netif g_netif;
static uint16_t g_rx_cur;
static uint16_t g_tx_cur;

static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint8_t  inb(uint16_t p) { uint8_t v;  __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static void wr_csr(uint16_t idx, uint32_t v) { outl(g_io + REG_RAP, idx); outl(g_io + REG_RDP, v); }
static uint32_t rd_csr(uint16_t idx) { outl(g_io + REG_RAP, idx); return inl(g_io + REG_RDP); }

static uint32_t bcnt12(uint32_t bufsize) { return (uint32_t)((0u - bufsize) & 0xFFFu); }

static err_t pcnet_linkout(struct netif *nif, struct pbuf *p) {
    (void)nif;
    if (p == NULL) return ERR_VAL;
    uint16_t slot = g_tx_cur;
    uint8_t *buf = g_txbuf[slot];
    uint16_t copied = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if ((uint32_t)copied + q->len > BUF_SIZE) return ERR_BUF;
        memcpy(buf + copied, q->payload, q->len);
        copied = (uint16_t)(copied + q->len);
    }
    if (copied < 60) copied = 60;

    g_txd[slot].base    = (uint32_t)(uintptr_t)buf;
    g_txd[slot].msg_len = 0;
    g_txd[slot].resv    = 0;
    g_txd[slot].flags   = DESC_OWN | DESC_STP | DESC_ENP | DESC_ONES | bcnt12(copied);

    g_tx_cur = (uint16_t)((slot + 1u) % NTXD);
    wr_csr(0, rd_csr(0) | CSR0_TDMD);  /* demand transmit */
    return ERR_OK;
}

static err_t pcnet_netif_init(struct netif *nif) {
    nif->name[0] = 'p';
    nif->name[1] = 'c';
    nif->output     = etharp_output;
    nif->linkoutput = pcnet_linkout;
    nif->mtu        = 1500;
    nif->hwaddr_len = CANBOOT_NET_MAC_LEN;
    memcpy(nif->hwaddr, g_mac, CANBOOT_NET_MAC_LEN);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void pcnet_pump(void) {
    if (!g_present) return;
    while (!(g_rxd[g_rx_cur].flags & DESC_OWN)) {
        uint32_t f = g_rxd[g_rx_cur].flags;
        uint32_t mlen = g_rxd[g_rx_cur].msg_len & 0xFFFu;
        if ((f & (DESC_STP | DESC_ENP)) == (DESC_STP | DESC_ENP) &&
            !(f & DESC_ERR) && mlen > 4u && mlen <= BUF_SIZE) {
            uint16_t frame = (uint16_t)(mlen - 4u);  /* strip FCS */
            struct pbuf *pb = pbuf_alloc(PBUF_RAW, frame, PBUF_RAM);
            if (pb) {
                pbuf_take(pb, g_rxbuf[g_rx_cur], frame);
                if (g_netif.input(pb, &g_netif) != ERR_OK) pbuf_free(pb);
            }
        }
        g_rxd[g_rx_cur].msg_len = 0;
        g_rxd[g_rx_cur].flags   = DESC_OWN | DESC_ONES | bcnt12(BUF_SIZE);
        g_rx_cur = (uint16_t)((g_rx_cur + 1u) % NRXD);
    }
}

static const uint8_t *pcnet_mac(void) { return g_mac; }
static struct netif *pcnet_netif(void) { return &g_netif; }

static bool pcnet_init(void) {
    const struct canboot_pci_dev *devs = hal_pci_devs();
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *match = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].vendor == PCNET_VENDOR &&
            (devs[i].device == 0x2000u || devs[i].device == 0x2001u)) {
            match = &devs[i];
            break;
        }
    }
    if (!match) return false;

    hal_pci_enable_bus_master(match->addr);
    uint64_t bar0 = hal_pci_bar_addr(match->addr, 0);
    if (!bar0) return false;
    g_io = (uint16_t)(bar0 & ~0x3ull);

    /* Init block + descriptor rings carry 32-bit addresses; refuse if any
     * sit above 4 GiB rather than truncate and DMA into the wrong page. */
    if (((uintptr_t)g_rxd >> 32) || ((uintptr_t)g_txd >> 32) ||
        ((uintptr_t)&g_initblk >> 32) ||
        ((uintptr_t)g_rxbuf >> 32) || ((uintptr_t)g_txbuf >> 32)) {
        return false;
    }

    (void)inw(g_io + 0x14u);            /* WIO reset */
    for (int i = 0; i < CANBOOT_NET_MAC_LEN; i++)
        g_mac[i] = inb(g_io + REG_APROM + (uint16_t)i);

    outl(g_io + REG_RDP, 0);            /* enter 32-bit DWIO mode */
    wr_csr(58, (rd_csr(58) & 0xFF00u) | 2u);  /* SWSTYLE 2 */
    wr_csr(0, CSR0_STOP);

    for (uint16_t i = 0; i < NRXD; i++) {
        g_rxd[i].base    = (uint32_t)(uintptr_t)g_rxbuf[i];
        g_rxd[i].msg_len = 0;
        g_rxd[i].resv    = 0;
        g_rxd[i].flags   = DESC_OWN | DESC_ONES | bcnt12(BUF_SIZE);
    }
    for (uint16_t i = 0; i < NTXD; i++) {
        g_txd[i].base = (uint32_t)(uintptr_t)g_txbuf[i];
        g_txd[i].flags = 0;
        g_txd[i].msg_len = 0;
        g_txd[i].resv = 0;
    }

    memset(&g_initblk, 0, sizeof(g_initblk));
    g_initblk.mode = 0;
    g_initblk.rlen = (uint8_t)(LOG2_8 << 4);
    g_initblk.tlen = (uint8_t)(LOG2_8 << 4);
    memcpy(g_initblk.phys, g_mac, CANBOOT_NET_MAC_LEN);
    g_initblk.rdra = (uint32_t)(uintptr_t)g_rxd;
    g_initblk.tdra = (uint32_t)(uintptr_t)g_txd;

    uint32_t ib = (uint32_t)(uintptr_t)&g_initblk;
    wr_csr(1, ib & 0xFFFFu);
    wr_csr(2, (ib >> 16) & 0xFFFFu);

    g_rx_cur = 0;
    g_tx_cur = 0;

    wr_csr(0, CSR0_INIT);
    for (int i = 0; i < 1000000; i++)
        if (rd_csr(0) & CSR0_IDON) break;
    wr_csr(0, CSR0_STRT);               /* run, interrupts disabled (polled) */

    ip4_addr_t any = { 0 };
    netif_add(&g_netif, &any, &any, &any, NULL, pcnet_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    g_present = true;
    return true;
}

const struct canboot_nic canboot_nic_pcnet = {
    .name  = "pcnet",
    .init  = pcnet_init,
    .pump  = pcnet_pump,
    .mac   = pcnet_mac,
    .netif = pcnet_netif,
};

#endif /* __x86_64__ */
