/*
 * Milestone 6 self-test: bring lwIP up over the virtio-net driver, wait
 * for a DHCP lease, send a UDP echo to the QEMU SLIRP host
 * (10.0.2.2:7777), then issue a tiny HTTP GET to 10.0.2.2:8080 and
 * verify "canboot-hello" comes back.
 *
 * NO_SYS=1, lwIP raw API, polling pump from the main loop.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"

#include "hal/net.h"

bool canboot_virtio_net_init(void);
struct netif *canboot_virtio_net_netif(void);
uint64_t canboot_tsc_hz(void);

static inline uint64_t arch_now(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#endif
}

static inline void arch_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#endif
}

static void pump_for(uint32_t ms) {
    uint64_t hz = canboot_tsc_hz();
    uint64_t deadline = arch_now() + (hz * ms) / 1000ull;
    while (arch_now() < deadline) {
        hal_net_pump();
        sys_check_timeouts();
        arch_relax();
    }
}

static bool wait_for(volatile bool *flag, uint32_t timeout_ms) {
    uint64_t hz = canboot_tsc_hz();
    uint64_t deadline = arch_now() + (hz * timeout_ms) / 1000ull;
    while (!*flag && arch_now() < deadline) {
        hal_net_pump();
        sys_check_timeouts();
        arch_relax();
    }
    return *flag;
}

/* ---- UDP echo client --------------------------------------------------- */

static volatile bool g_udp_done;
static char          g_udp_rxbuf[64];
static uint16_t      g_udp_rxlen;

static void udp_rx_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *from, u16_t port) {
    (void)arg; (void)pcb; (void)from; (void)port;
    if (!p) return;
    uint16_t n = p->tot_len;
    if (n > sizeof(g_udp_rxbuf)) n = sizeof(g_udp_rxbuf);
    pbuf_copy_partial(p, g_udp_rxbuf, n, 0);
    g_udp_rxlen = n;
    g_udp_done = true;
    pbuf_free(p);
}

static bool run_udp_echo(void) {
    struct udp_pcb *pcb = udp_new();
    if (!pcb) return false;
    if (udp_bind(pcb, IP4_ADDR_ANY, 0) != ERR_OK) {
        udp_remove(pcb);
        return false;
    }
    udp_recv(pcb, udp_rx_cb, NULL);

    ip_addr_t dst;
    IP4_ADDR(&dst, 10, 0, 2, 2);

    static const char payload[] = "canboot-udp";
    const u16_t plen = (u16_t)(sizeof(payload) - 1);

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, plen, PBUF_RAM);
    if (!p) { udp_remove(pcb); return false; }
    memcpy(p->payload, payload, plen);

    g_udp_done = false;
    g_udp_rxlen = 0;
    err_t e = udp_sendto(pcb, p, &dst, 7777);
    pbuf_free(p);
    if (e != ERR_OK) { udp_remove(pcb); return false; }

    bool ok = wait_for(&g_udp_done, 3000);
    udp_remove(pcb);

    if (!ok) {
        printf("selftest: FAIL udp echo timeout\n");
        return false;
    }
    if (g_udp_rxlen != plen || memcmp(g_udp_rxbuf, payload, plen) != 0) {
        printf("selftest: FAIL udp echo mismatch len=%u\n",
               (unsigned)g_udp_rxlen);
        return false;
    }
    printf("selftest: udp echo ok (%u bytes)\n", (unsigned)plen);
    return true;
}

/* ---- TCP HTTP GET client ---------------------------------------------- */

static volatile bool g_tcp_done;
static volatile bool g_tcp_fail;
static char          g_tcp_body[128];
static uint16_t      g_tcp_body_len;
static bool          g_tcp_headers_done;
static int           g_tcp_status;

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK) { g_tcp_fail = true; g_tcp_done = true; return ERR_OK; }
    if (!p) {
        /* remote closed */
        g_tcp_done = true;
        tcp_close(pcb);
        return ERR_OK;
    }
    char tmp[256];
    uint16_t n = p->tot_len;
    if (n > sizeof(tmp)) n = sizeof(tmp);
    pbuf_copy_partial(p, tmp, n, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    for (uint16_t i = 0; i < n; i++) {
        if (!g_tcp_headers_done) {
            static char hdr[256];
            static uint16_t hdr_n;
            if (hdr_n < sizeof(hdr) - 1) hdr[hdr_n++] = tmp[i];
            if (hdr_n >= 4 &&
                hdr[hdr_n - 4] == '\r' && hdr[hdr_n - 3] == '\n' &&
                hdr[hdr_n - 2] == '\r' && hdr[hdr_n - 1] == '\n') {
                g_tcp_headers_done = true;
                /* parse status code: "HTTP/1.x NNN " */
                if (hdr_n >= 12) {
                    g_tcp_status = (hdr[9] - '0') * 100
                                 + (hdr[10] - '0') * 10
                                 + (hdr[11] - '0');
                }
            }
        } else {
            if (g_tcp_body_len < sizeof(g_tcp_body) - 1) {
                g_tcp_body[g_tcp_body_len++] = tmp[i];
            }
        }
    }
    return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        g_tcp_fail = true; g_tcp_done = true;
        return err;
    }
    static const char req[] =
        "GET / HTTP/1.1\r\n"
        "Host: canboot\r\n"
        "Connection: close\r\n"
        "\r\n";
    err_t e = tcp_write(pcb, req, sizeof(req) - 1, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK) tcp_output(pcb);
    return e;
}

static void tcp_err_cb(void *arg, err_t err) {
    (void)arg; (void)err;
    g_tcp_fail = true; g_tcp_done = true;
}

static bool run_http_get(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return false;

    g_tcp_done = false;
    g_tcp_fail = false;
    g_tcp_body_len = 0;
    g_tcp_headers_done = false;
    g_tcp_status = 0;

    tcp_recv(pcb, tcp_recv_cb);
    tcp_err(pcb, tcp_err_cb);

    ip_addr_t dst;
    IP4_ADDR(&dst, 10, 0, 2, 2);
    if (tcp_connect(pcb, &dst, 8080, tcp_connected_cb) != ERR_OK) {
        tcp_abort(pcb);
        return false;
    }

    if (!wait_for(&g_tcp_done, 5000)) {
        printf("selftest: FAIL http timeout\n");
        return false;
    }
    if (g_tcp_fail) {
        printf("selftest: FAIL http transport\n");
        return false;
    }
    g_tcp_body[g_tcp_body_len] = '\0';
    if (g_tcp_status != 200) {
        printf("selftest: FAIL http status=%d\n", g_tcp_status);
        return false;
    }
    if (strstr(g_tcp_body, "canboot-hello") == NULL) {
        printf("selftest: FAIL http body='%.32s'\n", g_tcp_body);
        return false;
    }
    printf("selftest: http get ok (status=%d body='%s')\n",
           g_tcp_status, g_tcp_body);
    return true;
}

/* ---- Entry ------------------------------------------------------------ */

void net_selftest(void) {
    printf("selftest: starting net test\n");

    lwip_init();

    if (!canboot_virtio_net_init()) {
        printf("selftest: virtio-net absent, skipping\n");
        return;
    }
    printf("selftest: virtio-net mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           hal_net_mac()[0], hal_net_mac()[1], hal_net_mac()[2],
           hal_net_mac()[3], hal_net_mac()[4], hal_net_mac()[5]);

    struct netif *nif = canboot_virtio_net_netif();
    printf("selftest: netif flags=0x%02x mtu=%u link_up=%d up=%d\n",
           (unsigned)nif->flags, (unsigned)nif->mtu,
           netif_is_link_up(nif), netif_is_up(nif));
    err_t derr = dhcp_start(nif);
    printf("selftest: dhcp_start err=%d sys_now=%u\n", (int)derr, (unsigned)sys_now());

    /* Wait up to 15s for a DHCP lease. */
    uint64_t hz = canboot_tsc_hz();
    uint64_t dl = arch_now() + hz * 15ull;
    bool got_lease = false;
    uint32_t last_log = 0;
    extern uint32_t canboot_virtio_net_stat_tx_calls(void);
    extern uint32_t canboot_virtio_net_stat_tx_kicked(void);
    extern uint32_t canboot_virtio_net_stat_rx_done(void);
    extern uint32_t canboot_virtio_net_stat_tx_done(void);

    while (arch_now() < dl) {
        hal_net_pump();
        sys_check_timeouts();
        if (dhcp_supplied_address(nif)) { got_lease = true; break; }
        uint32_t now = sys_now() / 1000;
        if (now != last_log) {
            last_log = now;
            struct dhcp *d = netif_dhcp_data(nif);
            printf("selftest: dhcp t=%us state=%d tries=%u tx_call=%u tx_kick=%u tx_done=%u rx_done=%u\n",
                   (unsigned)now,
                   d ? (int)d->state : -1,
                   d ? (unsigned)d->tries : 0u,
                   (unsigned)canboot_virtio_net_stat_tx_calls(),
                   (unsigned)canboot_virtio_net_stat_tx_kicked(),
                   (unsigned)canboot_virtio_net_stat_tx_done(),
                   (unsigned)canboot_virtio_net_stat_rx_done());
        }
        arch_relax();
    }
    if (!got_lease) {
        printf("selftest: FAIL dhcp timeout\n");
        return;
    }
    char ip[16], gw[16], nm[16];
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
             ip4_addr1(netif_ip4_addr(nif)),
             ip4_addr2(netif_ip4_addr(nif)),
             ip4_addr3(netif_ip4_addr(nif)),
             ip4_addr4(netif_ip4_addr(nif)));
    snprintf(gw, sizeof(gw), "%d.%d.%d.%d",
             ip4_addr1(netif_ip4_gw(nif)),
             ip4_addr2(netif_ip4_gw(nif)),
             ip4_addr3(netif_ip4_gw(nif)),
             ip4_addr4(netif_ip4_gw(nif)));
    snprintf(nm, sizeof(nm), "%d.%d.%d.%d",
             ip4_addr1(netif_ip4_netmask(nif)),
             ip4_addr2(netif_ip4_netmask(nif)),
             ip4_addr3(netif_ip4_netmask(nif)),
             ip4_addr4(netif_ip4_netmask(nif)));
    printf("selftest: dhcp lease ip=%s gw=%s mask=%s\n", ip, gw, nm);

    /* Let ARP settle and any pending DHCP traffic flush. */
    pump_for(500);

    if (!run_udp_echo()) return;
    if (!run_http_get())  return;

    printf("selftest: net test ok\n");
}
