/*
 * Intel 8254x ("e1000") driver wired into lwIP's NO_SYS netif. Covers the
 * 82540EM / 82545EM family that QEMU's `-device e1000` and VirtualBox's
 * default NIC present (PCI 8086:100E and friends). Legacy descriptor
 * rings, MMIO BAR0, MAC from EEPROM. No interrupts: RX/TX are polled from
 * hal_net_pump(), matching the cooperative scheduler.
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

#define E1000_VENDOR 0x8086u

/* Register offsets (byte). */
#define E1000_CTRL   0x0000u
#define E1000_STATUS 0x0008u
#define E1000_EERD   0x0014u
#define E1000_ICR    0x00C0u
#define E1000_IMC    0x00D8u
#define E1000_RCTL   0x0100u
#define E1000_TCTL   0x0400u
#define E1000_TIPG   0x0410u
#define E1000_RDBAL  0x2800u
#define E1000_RDBAH  0x2804u
#define E1000_RDLEN  0x2808u
#define E1000_RDH    0x2810u
#define E1000_RDT    0x2818u
#define E1000_TDBAL  0x3800u
#define E1000_TDBAH  0x3804u
#define E1000_TDLEN  0x3808u
#define E1000_TDH    0x3810u
#define E1000_TDT    0x3818u
#define E1000_MTA    0x5200u
#define E1000_RAL    0x5400u
#define E1000_RAH    0x5404u

#define CTRL_SLU   0x00000040u  /* set link up           */
#define CTRL_ASDE  0x00000020u  /* auto speed detect     */
#define CTRL_RST   0x04000000u  /* device reset          */
#define CTRL_LRST  0x00000008u
#define CTRL_PHY_RST 0x80000000u
#define CTRL_ILOS  0x00000080u
#define CTRL_VME   0x40000000u

#define EERD_START 0x00000001u
#define EERD_DONE  0x00000010u

#define RCTL_EN    0x00000002u
#define RCTL_BAM   0x00008000u  /* broadcast accept      */
#define RCTL_SECRC 0x04000000u  /* strip CRC             */
/* RCTL.BSIZE 00b + no BSEX => 2048-byte buffers. */

#define TCTL_EN    0x00000002u
#define TCTL_PSP   0x00000008u  /* pad short packets     */
#define TCTL_CT    (0x10u << 4)     /* collision threshold */
#define TCTL_COLD  (0x40u << 12)    /* collision distance (FD) */

#define RX_STAT_DD  0x01u
#define RX_STAT_EOP 0x02u
#define TX_CMD_EOP  0x01u
#define TX_CMD_IFCS 0x02u
#define TX_CMD_RS   0x08u

#define NRXD     16u
#define NTXD     16u
#define BUF_SIZE 2048u

struct __attribute__((packed)) e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
};

struct __attribute__((packed)) e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
};

static __attribute__((aligned(128))) struct e1000_rx_desc g_rxd[NRXD];
static __attribute__((aligned(128))) struct e1000_tx_desc g_txd[NTXD];
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

static int e1000_match(uint16_t dev) {
    static const uint16_t ids[] = {
        0x100Eu, 0x100Fu, 0x1010u, 0x1011u, 0x1012u, 0x1004u, 0x100Cu,
        0x100Du, 0x1015u, 0x1016u, 0x1017u, 0x1026u, 0x1027u, 0x1028u,
        0x1019u, 0x101Au, 0x1013u, 0x1018u,
    };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (ids[i] == dev) return 1;
    return 0;
}

static uint16_t eeprom_read(uint8_t word) {
    e_wr(E1000_EERD, ((uint32_t)word << 8) | EERD_START);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = e_rd(E1000_EERD);
        if (v & EERD_DONE) return (uint16_t)(v >> 16);
    }
    return 0;
}

static void read_mac(void) {
    for (int w = 0; w < 3; w++) {
        uint16_t v = eeprom_read((uint8_t)w);
        g_mac[w * 2]     = (uint8_t)(v & 0xFF);
        g_mac[w * 2 + 1] = (uint8_t)(v >> 8);
    }
    /* If the EEPROM path yielded nothing usable, fall back to RAL/RAH
     * (firmware may have pre-loaded the receive address). */
    int empty = 1;
    for (int i = 0; i < CANBOOT_NET_MAC_LEN; i++) if (g_mac[i]) empty = 0;
    if (empty) {
        uint32_t ral = e_rd(E1000_RAL), rah = e_rd(E1000_RAH);
        g_mac[0] = (uint8_t)ral;        g_mac[1] = (uint8_t)(ral >> 8);
        g_mac[2] = (uint8_t)(ral >> 16); g_mac[3] = (uint8_t)(ral >> 24);
        g_mac[4] = (uint8_t)rah;        g_mac[5] = (uint8_t)(rah >> 8);
    }
}

static err_t e1000_linkout(struct netif *nif, struct pbuf *p) {
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
    e_wr(E1000_TDT, g_tx_cur);
    return ERR_OK;
}

static err_t e1000_netif_init(struct netif *nif) {
    nif->name[0] = 'e';
    nif->name[1] = 'n';
    nif->output     = etharp_output;
    nif->linkoutput = e1000_linkout;
    nif->mtu        = 1500;
    nif->hwaddr_len = CANBOOT_NET_MAC_LEN;
    memcpy(nif->hwaddr, g_mac, CANBOOT_NET_MAC_LEN);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void e1000_pump(void) {
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
        e_wr(E1000_RDT, g_rx_cur);              /* hand the buffer back */
        g_rx_cur = (uint16_t)((g_rx_cur + 1u) % NRXD);
    }
}

static const uint8_t *e1000_mac(void) { return g_mac; }
static struct netif *e1000_netif(void) { return &g_netif; }

static bool e1000_init(void) {
    const struct canboot_pci_dev *devs = hal_pci_devs();
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *match = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].vendor == E1000_VENDOR && e1000_match(devs[i].device)) {
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

    e_wr(E1000_IMC, 0xFFFFFFFFu);               /* mask all interrupts */
    e_wr(E1000_CTRL, e_rd(E1000_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++)
        if (!(e_rd(E1000_CTRL) & CTRL_RST)) break;
    e_wr(E1000_IMC, 0xFFFFFFFFu);
    (void)e_rd(E1000_ICR);

    uint32_t ctrl = e_rd(E1000_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_VME);
    e_wr(E1000_CTRL, ctrl);

    read_mac();

    for (uint32_t i = 0; i < 128u; i++) e_wr(E1000_MTA + i * 4u, 0);

    /* RX ring. */
    for (uint16_t i = 0; i < NRXD; i++) {
        g_rxd[i].addr   = (uint64_t)(uintptr_t)g_rxbuf[i];
        g_rxd[i].status = 0;
    }
    e_wr(E1000_RDBAL, (uint32_t)((uintptr_t)g_rxd & 0xFFFFFFFFu));
    e_wr(E1000_RDBAH, (uint32_t)((uint64_t)(uintptr_t)g_rxd >> 32));
    e_wr(E1000_RDLEN, NRXD * sizeof(struct e1000_rx_desc));
    e_wr(E1000_RDH, 0);
    e_wr(E1000_RDT, NRXD - 1u);
    g_rx_cur = 0;
    e_wr(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* TX ring. */
    for (uint16_t i = 0; i < NTXD; i++) {
        g_txd[i].addr   = (uint64_t)(uintptr_t)g_txbuf[i];
        g_txd[i].status = 0;
    }
    e_wr(E1000_TDBAL, (uint32_t)((uintptr_t)g_txd & 0xFFFFFFFFu));
    e_wr(E1000_TDBAH, (uint32_t)((uint64_t)(uintptr_t)g_txd >> 32));
    e_wr(E1000_TDLEN, NTXD * sizeof(struct e1000_tx_desc));
    e_wr(E1000_TDH, 0);
    e_wr(E1000_TDT, 0);
    g_tx_cur = 0;
    e_wr(E1000_TIPG, 0x0060200Au);
    e_wr(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    ip4_addr_t any = { 0 };
    netif_add(&g_netif, &any, &any, &any, NULL, e1000_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    g_present = true;
    return true;
}

const struct canboot_nic canboot_nic_e1000 = {
    .name  = "e1000",
    .init  = e1000_init,
    .pump  = e1000_pump,
    .mac   = e1000_mac,
    .netif = e1000_netif,
};
