/*
 * PXE/netboot DHCP capture. lwIP calls canboot_dhcp_parse_option() for every
 * DHCP option in an OFFER/ACK that it doesn't handle internally; the full
 * BOOTP message (with siaddr) is handed in too. We record the TFTP server so
 * the init.cdo loader can fetch the boot script over TFTP on a diskless PXE
 * client. Pure port code - no vendored lwIP source is modified.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lwip/prot/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/def.h"

#include "netboot_hooks.h"

/* DHCP options lwIP does not consume itself, so they reach the hook. */
#define DHCP_OPT_TFTP_SERVER_NAME   66u   /* string: TFTP server host/IP    */
#define DHCP_OPT_TFTP_SERVER_ADDR   150u  /* IP address list: TFTP server(s) */

#define DHCP_MSG_OFFER              2u
#define DHCP_MSG_ACK                5u

/* Source ranking: a stronger hint within the same reply wins, and a later
 * reply (the ACK) can replace whatever the OFFER left behind. */
enum { SRC_NONE = 0, SRC_SIADDR, SRC_OPT66, SRC_OPT150 };

static u32_t g_addr;   /* network byte order */
static int   g_src;

static void consider(u32_t addr_net, int src) {
    if (addr_net == 0) return;
    if (src < g_src) return;
    g_addr = addr_net;
    g_src  = src;
}

/* Parse "a.b.c.d" (option 66 is a name, but PXE servers commonly put a literal
 * IP there; DNS resolution isn't available this early in the bootstrap). */
static bool parse_dotted_quad(const char *s, u16_t len, u32_t *out_net) {
    uint32_t parts[4] = {0};
    int idx = 0, cur = -1;
    for (u16_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (cur < 0) cur = 0;
            cur = cur * 10 + (c - '0');
            if (cur > 255) return false;
        } else if (c == '.') {
            if (cur < 0 || idx >= 3) return false;
            parts[idx++] = (uint32_t)cur;
            cur = -1;
        } else if (c == '\0') {
            break;
        } else {
            return false;
        }
    }
    if (idx != 3 || cur < 0) return false;
    parts[3] = (uint32_t)cur;
    *out_net = lwip_htonl((parts[0] << 24) | (parts[1] << 16) |
                          (parts[2] << 8)  |  parts[3]);
    return true;
}

void canboot_dhcp_parse_option(struct dhcp_msg *msg, u8_t msg_type,
                               u8_t option, u8_t len,
                               struct pbuf *p, u16_t offset) {
    /* msg_type is only valid once option 53 has been parsed; 0 means "not yet
     * known", which still happens during a valid OFFER/ACK. */
    if (msg_type != 0 && msg_type != DHCP_MSG_OFFER && msg_type != DHCP_MSG_ACK)
        return;

    /* siaddr (next-server) is in the BOOTP header, always present. */
    if (msg)
        consider(msg->siaddr.addr, SRC_SIADDR);

    if (option == DHCP_OPT_TFTP_SERVER_ADDR && len >= 4 && p) {
        u8_t b[4];
        if (pbuf_copy_partial(p, b, 4, offset) == 4) {
            u32_t a;
            memcpy(&a, b, 4);   /* wire bytes are already network order */
            consider(a, SRC_OPT150);
        }
    } else if (option == DHCP_OPT_TFTP_SERVER_NAME && len > 0 && p) {
        char name[64];
        u16_t n = (len < sizeof(name) - 1) ? len : (u16_t)(sizeof(name) - 1);
        if (pbuf_copy_partial(p, name, n, offset) == n) {
            name[n] = '\0';
            u32_t a;
            if (parse_dotted_quad(name, n, &a))
                consider(a, SRC_OPT66);
        }
    }
}

int canboot_netboot_tftp_server(ip_addr_t *out) {
    if (g_src == SRC_NONE || g_addr == 0)
        return 0;
    if (out)
        ip_addr_set_ip4_u32(out, g_addr);
    return 1;
}
