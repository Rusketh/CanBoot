/*
 * Intel 8257x ("e1000e") driver wired into lwIP's NO_SYS netif. Covers the
 * PCIe gigabit family QEMU's `-device e1000e` (82574L, PCI 8086:10D3)
 * presents, and the 82571/82572/82573/82574/82583 and a few client
 * (I217/I218/I219) device ids found on real hardware. Legacy descriptor
 * rings, MMIO BAR0, MAC from the receive-address registers. No interrupts:
 * RX/TX are polled from hal_net_pump(), matching the cooperative scheduler.
 *
 * The register file and legacy descriptor layout are the same as the older
 * 8254x e1000; the differences that matter here are that the e1000e EEPROM
 * (EERD) access format differs - so the MAC is taken straight from RAL/RAH
 * (firmware/QEMU preloads it) - and the per-queue RXDCTL/TXDCTL enable bits
 * are set explicitly, which the PCIe parts expect.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hal/net.h"
#include "hal/pci.h"

#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#define E1000E_VENDOR 0x8086u

/* Register offsets (byte). */
#define E1000E_CTRL    0x0000u
#define E1000E_STATUS  0x0008u
#define E1000E_CTRLEXT 0x0018u
#define E1000E_ICR     0x00C0u
#define E1000E_IMC     0x00D8u
#define E1000E_RCTL    0x0100u
#define E1000E_TCTL    0x0400u
#define E1000E_TIPG    0x0410u
#define E1000E_RDBAL   0x2800u
#define E1000E_RDBAH   0x2804u
#define E1000E_RDLEN   0x2808u
#define E1000E_RDH     0x2810u
#define E1000E_RDT     0x2818u
#define E1000E_RXDCTL  0x2828u
#define E1000E_TDBAL   0x3800u
#define E1000E_TDBAH   0x3804u
#define E1000E_TDLEN   0x3808u
#define E1000E_TDH     0x3810u
#define E1000E_TDT     0x3818u
#define E1000E_TXDCTL  0x3828u
#define E1000E_MTA     0x5200u
#define E1000E_RAL     0x5400u
#define E1000E_RAH     0x5404u

#define CTRL_SLU     0x00000040u  /* set link up           */
#define CTRL_ASDE    0x00000020u  /* auto speed detect     */
#define CTRL_RST     0x04000000u  /* device reset          */
#define CTRL_LRST    0x00000008u
#define CTRL_PHY_RST 0x80000000u
#define CTRL_ILOS    0x00000080u
#define CTRL_VME     0x40000000u

#define RCTL_EN    0x00000002u
#define RCTL_BAM   0x00008000u  /* broadcast accept      */
#define RCTL_SECRC 0x04000000u  /* strip CRC             */
/* RCTL.BSIZE 00b + no BSEX => 2048-byte buffers. */

#define TCTL_EN    0x00000002u
#define TCTL_PSP   0x00000008u  /* pad short packets     */
#define TCTL_CT    (0x10u << 4)     /* collision threshold */
#define TCTL_COLD  (0x40u << 12)    /* collision distance (FD) */

#define XDCTL_ENABLE 0x02000000u /* RXDCTL/TXDCTL queue enable (bit 25) */

#define RAH_AV     0x80000000u  /* receive address valid */

#define RX_STAT_DD  0x01u
#define RX_STAT_EOP 0x02u
#define TX_CMD_EOP  0x01u
#define TX_CMD_IFCS 0x02u
#define TX_CMD_RS   0x08u

#define NRXD     16u
#define NTXD     16u
#define BUF_SIZE 2048u

struct __attribute__((packed)) e1000e_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
};

struct __attribute__((packed)) e1000e_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
};

static __attribute__((aligned(128))) struct e1000e_rx_desc g_rxd[NRXD];
static __attribute__((aligned(128))) struct e1000e_tx_desc g_txd[NTXD];
static __attribute__((aligned(16)))  uint8_t g_rxbuf[NRXD][BUF_SIZE];
static __attribute__((aligned(16)))  uint8_t g_txbuf[NTXD][BUF_SIZE];

static volatile uint8_t *g_mmio;
static bool     g_present;
static uint8_t  g_mac[CANBOOT_NET_MAC_LEN];
static struct   netif g_netif;
static uint16_t g_rx_cur;
static uint16_t g_tx_cur;

static inline uint32_t e_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_mmio + off);
}
static inline void e_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_mmio + off) = v;
}

static int e1000e_match(uint16_t dev) {
    static const uint16_t ids[] = {
        0x10D3u,                            /* 82574L  (QEMU -device e1000e) */
        0x105Eu, 0x105Fu, 0x1060u,          /* 82571   */
        0x10A4u, 0x10A5u, 0x10BCu, 0x10D9u, 0x10DAu,
        0x107Du, 0x107Eu, 0x107Fu, 0x10B9u, /* 82572/82573 */
        0x108Bu, 0x108Cu, 0x109Au,          /* 82573   */
        0x10F6u, 0x150Cu,                   /* 82574/82583 */
        0x10EAu, 0x10EBu, 0x10EFu, 0x10F0u, /* I217/I218 family */
        0x1502u, 0x1503u,                   /* 82579   */
        0x15A0u, 0x15A1u, 0x15A2u, 0x15A3u, /* I218    */
        0x156Fu, 0x1570u, 0x15B7u, 0x15B8u, 0x15D7u, 0x15D8u, /* I219 */
    };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (ids[i] == dev) return 1;
    return 0;
}

/* The 82574 EEPROM access differs from the 8254x and QEMU preloads the
 * receive-address registers, so take the MAC straight from RAL/RAH. */
static void read_mac(void) {
    uint32_t ral = e_rd(E1000E_RAL), rah = e_rd(E1000E_RAH);
    g_mac[0] = (uint8_t)ral;         g_mac[1] = (uint8_t)(ral >> 8);
    g_mac[2] = (uint8_t)(ral >> 16); g_mac[3] = (uint8_t)(ral >> 24);
    g_mac[4] = (uint8_t)rah;         g_mac[5] = (uint8_t)(rah >> 8);
}

static err_t e1000e_linkout(struct netif *nif, struct pbuf *p) {
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
    g_txd[slot].addr   = (uint64_t)(uintptr_t)buf;
    g_txd[slot].length = copied;
    g_txd[slot].cmd    = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    g_txd[slot].status = 0;

    g_tx_cur = (uint16_t)((slot + 1u) % NTXD);
    e_wr(E1000E_TDT, g_tx_cur);
    return ERR_OK;
}

static err_t e1000e_netif_init(struct netif *nif) {
    nif->name[0] = 'e';
    nif->name[1] = 'n';
    nif->output     = etharp_output;
    nif->linkoutput = e1000e_linkout;
    nif->mtu        = 1500;
    nif->hwaddr_len = CANBOOT_NET_MAC_LEN;
    memcpy(nif->hwaddr, g_mac, CANBOOT_NET_MAC_LEN);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void e1000e_pump(void) {
    if (!g_present) return;
    while (g_rxd[g_rx_cur].status & RX_STAT_DD) {
        uint16_t len = g_rxd[g_rx_cur].length;
        if ((g_rxd[g_rx_cur].status & RX_STAT_EOP) && len > 0 && len <= BUF_SIZE) {
            struct pbuf *pb = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
            if (pb) {
                pbuf_take(pb, g_rxbuf[g_rx_cur], len);
                if (g_netif.input(pb, &g_netif) != ERR_OK) pbuf_free(pb);
            }
        }
        g_rxd[g_rx_cur].status = 0;
        e_wr(E1000E_RDT, g_rx_cur);             /* hand the buffer back */
        g_rx_cur = (uint16_t)((g_rx_cur + 1u) % NRXD);
    }
}

static const uint8_t *e1000e_mac(void) { return g_mac; }
static struct netif *e1000e_netif(void) { return &g_netif; }

static bool e1000e_init(void) {
    const struct canboot_pci_dev *devs = hal_pci_devs();
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *match = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].vendor == E1000E_VENDOR && e1000e_match(devs[i].device)) {
            match = &devs[i];
            break;
        }
    }
    if (!match) return false;
    if (!hal_pci_bar_is_mmio(match->addr, 0)) return false;
    uint64_t bar0 = hal_pci_bar_addr(match->addr, 0);
    if (!bar0) return false;

    hal_pci_enable_bus_master(match->addr);
    g_mmio = (volatile uint8_t *)(uintptr_t)bar0;

    e_wr(E1000E_IMC, 0xFFFFFFFFu);              /* mask all interrupts */
    e_wr(E1000E_CTRL, e_rd(E1000E_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++)
        if (!(e_rd(E1000E_CTRL) & CTRL_RST)) break;
    e_wr(E1000E_IMC, 0xFFFFFFFFu);
    (void)e_rd(E1000E_ICR);

    uint32_t ctrl = e_rd(E1000E_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_VME);
    e_wr(E1000E_CTRL, ctrl);

    read_mac();
    /* Make sure the receive-address filter is valid for our MAC. */
    e_wr(E1000E_RAL, (uint32_t)g_mac[0] | ((uint32_t)g_mac[1] << 8) |
                     ((uint32_t)g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24));
    e_wr(E1000E_RAH, (uint32_t)g_mac[4] | ((uint32_t)g_mac[5] << 8) | RAH_AV);

    for (uint32_t i = 0; i < 128u; i++) e_wr(E1000E_MTA + i * 4u, 0);

    /* RX ring. */
    for (uint16_t i = 0; i < NRXD; i++) {
        g_rxd[i].addr   = (uint64_t)(uintptr_t)g_rxbuf[i];
        g_rxd[i].status = 0;
    }
    e_wr(E1000E_RDBAL, (uint32_t)((uintptr_t)g_rxd & 0xFFFFFFFFu));
    e_wr(E1000E_RDBAH, (uint32_t)((uint64_t)(uintptr_t)g_rxd >> 32));
    e_wr(E1000E_RDLEN, NRXD * sizeof(struct e1000e_rx_desc));
    e_wr(E1000E_RDH, 0);
    e_wr(E1000E_RDT, NRXD - 1u);
    g_rx_cur = 0;
    e_wr(E1000E_RXDCTL, XDCTL_ENABLE);          /* enable RX queue 0 */
    e_wr(E1000E_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* TX ring. */
    for (uint16_t i = 0; i < NTXD; i++) {
        g_txd[i].addr   = (uint64_t)(uintptr_t)g_txbuf[i];
        g_txd[i].status = 0;
    }
    e_wr(E1000E_TDBAL, (uint32_t)((uintptr_t)g_txd & 0xFFFFFFFFu));
    e_wr(E1000E_TDBAH, (uint32_t)((uint64_t)(uintptr_t)g_txd >> 32));
    e_wr(E1000E_TDLEN, NTXD * sizeof(struct e1000e_tx_desc));
    e_wr(E1000E_TDH, 0);
    e_wr(E1000E_TDT, 0);
    g_tx_cur = 0;
    e_wr(E1000E_TIPG, 0x0060200Au);
    e_wr(E1000E_TXDCTL, XDCTL_ENABLE);          /* enable TX queue 0 */
    e_wr(E1000E_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    ip4_addr_t any = { 0 };
    netif_add(&g_netif, &any, &any, &any, NULL, e1000e_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    g_present = true;
    return true;
}

const struct canboot_nic canboot_nic_e1000e = {
    .name  = "e1000e",
    .init  = e1000e_init,
    .pump  = e1000e_pump,
    .mac   = e1000e_mac,
    .netif = e1000e_netif,
};
