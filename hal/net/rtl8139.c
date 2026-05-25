/*
 * Realtek RTL8139 driver wired into lwIP's NO_SYS netif. QEMU's
 * `-device rtl8139` and a common legacy/virtual NIC (PCI 10EC:8139).
 * Port-I/O BAR0, one linear RX ring + 4 round-robin TX descriptors, MAC
 * auto-loaded into IDR0-5 from EEPROM at reset. Polled from
 * hal_net_pump(). x86_64 only (legacy PIO).
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

#define RTL_VENDOR 0x10ECu
#define RTL_DEVICE 0x8139u

#define REG_IDR0    0x00u
#define REG_TSD0    0x10u  /* +4*i  */
#define REG_TSAD0   0x20u  /* +4*i  */
#define REG_RBSTART 0x30u
#define REG_CR      0x37u
#define REG_CAPR    0x38u
#define REG_IMR     0x3Cu
#define REG_ISR     0x3Eu
#define REG_TCR     0x40u
#define REG_RCR     0x44u
#define REG_CONFIG1 0x52u

#define CR_RST  0x10u
#define CR_RE   0x08u
#define CR_TE   0x04u
#define CR_BUFE 0x01u  /* RX buffer empty */

#define RX_STAT_ROK 0x0001u

#define RCR_AAP  0x00000001u
#define RCR_APM  0x00000002u
#define RCR_AM   0x00000004u
#define RCR_AB   0x00000008u
#define RCR_WRAP 0x00000080u

#define RX_RING   8192u                 /* RBLEN 00 = 8K */
#define RX_TOTAL  (RX_RING + 16u + 1536u) /* +pad so WRAP never splits a frame */
#define NTXD      4u
#define TX_SIZE   2048u

static __attribute__((aligned(4))) uint8_t g_rx[RX_TOTAL];
static __attribute__((aligned(4))) uint8_t g_tx[NTXD][TX_SIZE];

static uint16_t g_io;
static bool     g_present;
static uint8_t  g_mac[CANBOOT_NET_MAC_LEN];
static struct   netif g_netif;
static uint32_t g_rx_off;
static uint16_t g_tx_cur;

static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t v;  __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

static err_t rtl_linkout(struct netif *nif, struct pbuf *p) {
    (void)nif;
    if (p == NULL) return ERR_VAL;
    uint16_t slot = g_tx_cur;
    uint8_t *buf = g_tx[slot];
    uint16_t copied = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if ((uint32_t)copied + q->len > TX_SIZE) return ERR_BUF;
        memcpy(buf + copied, q->payload, q->len);
        copied = (uint16_t)(copied + q->len);
    }
    if (copied < 60) copied = 60;  /* min Ethernet frame; chip pads CRC */

    outl(g_io + REG_TSAD0 + slot * 4u, (uint32_t)(uintptr_t)buf);
    outl(g_io + REG_TSD0  + slot * 4u, copied);  /* ERTXTH=0, OWN cleared => start */
    g_tx_cur = (uint16_t)((slot + 1u) % NTXD);
    return ERR_OK;
}

static err_t rtl_netif_init(struct netif *nif) {
    nif->name[0] = 'r';
    nif->name[1] = 't';
    nif->output     = etharp_output;
    nif->linkoutput = rtl_linkout;
    nif->mtu        = 1500;
    nif->hwaddr_len = CANBOOT_NET_MAC_LEN;
    memcpy(nif->hwaddr, g_mac, CANBOOT_NET_MAC_LEN);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void rtl_pump(void) {
    if (!g_present) return;
    while (!(inb(g_io + REG_CR) & CR_BUFE)) {
        uint32_t hdr = *(volatile uint32_t *)(g_rx + g_rx_off);
        uint16_t status = (uint16_t)(hdr & 0xFFFFu);
        uint16_t len    = (uint16_t)((hdr >> 16) & 0xFFFFu);

        if ((status & RX_STAT_ROK) && len >= 4u && len <= (RX_RING)) {
            uint16_t frame = (uint16_t)(len - 4u);  /* drop the 4-byte CRC */
            struct pbuf *pb = pbuf_alloc(PBUF_RAW, frame, PBUF_RAM);
            if (pb) {
                pbuf_take(pb, g_rx + g_rx_off + 4u, frame);
                if (g_netif.input(pb, &g_netif) != ERR_OK) pbuf_free(pb);
            }
        }

        g_rx_off = (g_rx_off + len + 4u + 3u) & ~3u;
        g_rx_off %= RX_RING;
        outw(g_io + REG_CAPR, (uint16_t)(g_rx_off - 16u));
        outw(g_io + REG_ISR, 0x0005u);  /* ack ROK|RER */
    }
}

static const uint8_t *rtl_mac(void) { return g_mac; }
static struct netif *rtl_netif(void) { return &g_netif; }

static bool rtl_init(void) {
    const struct canboot_pci_dev *devs = hal_pci_devs();
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *match = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].vendor == RTL_VENDOR && devs[i].device == RTL_DEVICE) {
            match = &devs[i];
            break;
        }
    }
    if (!match) return false;

    hal_pci_enable_bus_master(match->addr);
    uint64_t bar0 = hal_pci_bar_addr(match->addr, 0);
    if (!bar0) return false;
    g_io = (uint16_t)(bar0 & ~0x3ull);  /* I/O BAR: low bits are flags */

    /* RBSTART/TSAD are 32-bit; refuse if our buffers sit above 4 GiB. */
    if (((uintptr_t)g_rx >> 32) != 0) return false;

    outb(g_io + REG_CONFIG1, 0x00);             /* power on */
    outb(g_io + REG_CR, CR_RST);                /* soft reset */
    for (int i = 0; i < 1000000; i++)
        if (!(inb(g_io + REG_CR) & CR_RST)) break;

    for (int i = 0; i < CANBOOT_NET_MAC_LEN; i++)
        g_mac[i] = inb(g_io + REG_IDR0 + (uint16_t)i);

    g_rx_off = 0;
    outl(g_io + REG_RBSTART, (uint32_t)(uintptr_t)g_rx);
    outw(g_io + REG_IMR, 0x0000);               /* poll, no interrupts */
    outl(g_io + REG_RCR, RCR_APM | RCR_AB | RCR_WRAP | (7u << 8) | (7u << 13));
    outl(g_io + REG_TCR, (3u << 24) /* IFG normal */ );
    outw(g_io + REG_CAPR, (uint16_t)(0u - 16u));
    outb(g_io + REG_CR, CR_RE | CR_TE);
    g_tx_cur = 0;

    ip4_addr_t any = { 0 };
    netif_add(&g_netif, &any, &any, &any, NULL, rtl_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    g_present = true;
    return true;
}

const struct canboot_nic canboot_nic_rtl8139 = {
    .name  = "rtl8139",
    .init  = rtl_init,
    .pump  = rtl_pump,
    .mac   = rtl_mac,
    .netif = rtl_netif,
};

#endif /* __x86_64__ */
