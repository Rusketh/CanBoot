/*
 * Read-only TFTP client over lwIP raw UDP. See net/tftp.h. Modeled on the
 * synchronous pump pattern in cando_port/lib/net.c (n_udp_echo): single
 * transfer at a time, driven from the caller's thread via hal_net_pump() +
 * sys_check_timeouts().
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/timeouts.h"

#include "hal/net.h"
#include "net/tftp.h"

#define TFTP_PORT       69
#define TFTP_BLOCK      512
#define TFTP_OP_RRQ     1
#define TFTP_OP_DATA    3
#define TFTP_OP_ACK     4
#define TFTP_OP_ERROR   5

#define TFTP_OVERALL_MS 10000   /* whole-transfer deadline   */
#define TFTP_RETX_MS    1000    /* retransmit when stalled   */
#define TFTP_MAX_RETX   5

extern uint64_t canboot_tsc_hz(void);

static inline uint64_t arch_now(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

static inline void arch_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#endif
}

/* One transfer's worth of state; single-threaded so a file-static is fine. */
static struct {
    char    *out;
    uint32_t cap;
    uint32_t len;
    uint16_t block;      /* highest block number stored                 */
    uint16_t srv_port;   /* server transfer-ID (port of first DATA)      */
    bool     have_tid;
    bool     progress;   /* a fresh DATA arrived since last check        */
    bool     done;
    bool     error;
} g_x;

static struct udp_pcb *g_pcb;

static void send_ack(const ip_addr_t *to, uint16_t port, uint16_t block) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM);
    if (!p) return;
    uint8_t b[4] = { 0, TFTP_OP_ACK, (uint8_t)(block >> 8), (uint8_t)block };
    pbuf_take(p, b, 4);
    udp_sendto(g_pcb, p, to, port);
    pbuf_free(p);
}

static void send_rrq(const ip_addr_t *srv, const char *fn) {
    char req[2 + 200 + 1 + 5 + 1];
    uint16_t fl = (uint16_t)strlen(fn);
    if (fl > 200) fl = 200;
    uint16_t n = 0;
    req[n++] = 0;
    req[n++] = TFTP_OP_RRQ;
    memcpy(&req[n], fn, fl); n += fl;
    req[n++] = 0;
    memcpy(&req[n], "octet", 5); n += 5;
    req[n++] = 0;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
    if (!p) return;
    pbuf_take(p, req, n);
    udp_sendto(g_pcb, p, srv, TFTP_PORT);
    pbuf_free(p);
}

static void rx_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                  const ip_addr_t *from, u16_t port) {
    (void)arg; (void)pcb;
    if (!p) return;

    uint8_t hdr[4];
    if (pbuf_copy_partial(p, hdr, 4, 0) < 4) { pbuf_free(p); return; }
    uint16_t op  = ((uint16_t)hdr[0] << 8) | hdr[1];
    uint16_t blk = ((uint16_t)hdr[2] << 8) | hdr[3];

    if (op == TFTP_OP_ERROR) { g_x.error = true; g_x.done = true; pbuf_free(p); return; }
    if (op != TFTP_OP_DATA)  { pbuf_free(p); return; }

    /* Lock onto the server's transfer-ID (port of the first DATA) and ignore
     * datagrams from any other source. */
    if (!g_x.have_tid) { g_x.srv_port = port; g_x.have_tid = true; }
    else if (port != g_x.srv_port) { pbuf_free(p); return; }

    uint16_t expect = (uint16_t)(g_x.block + 1);
    uint16_t dlen   = (p->tot_len > 4) ? (uint16_t)(p->tot_len - 4) : 0;

    if (blk == expect) {
        if (dlen) {
            uint32_t space = (g_x.cap > g_x.len) ? (g_x.cap - g_x.len) : 0;
            uint16_t take  = (dlen <= space) ? dlen : (uint16_t)space;
            if (take) pbuf_copy_partial(p, g_x.out + g_x.len, take, 4);
            g_x.len += take;
            if (take < dlen) { g_x.error = true; g_x.done = true; }  /* overflow */
        }
        g_x.block    = expect;
        g_x.progress = true;
        send_ack(from, port, expect);
        if (!g_x.error && dlen < TFTP_BLOCK) g_x.done = true;        /* EOF */
    } else {
        /* Duplicate/out-of-order: re-ACK the last good block to resync. */
        send_ack(from, port, g_x.block);
    }
    pbuf_free(p);
}

int canboot_tftp_get(const ip_addr_t *server, const char *filename,
                     char *out, uint32_t out_cap, uint32_t *out_len) {
    if (!server || !filename || !out || out_cap == 0) return -1;

    memset(&g_x, 0, sizeof g_x);
    g_x.out = out;
    g_x.cap = out_cap;

    g_pcb = udp_new();
    if (!g_pcb) return -1;
    udp_recv(g_pcb, rx_cb, NULL);

    send_rrq(server, filename);

    uint64_t hz = canboot_tsc_hz();
    uint64_t overall_dl = arch_now() + (hz * TFTP_OVERALL_MS) / 1000ull;
    uint64_t retx_at    = arch_now() + (hz * TFTP_RETX_MS)    / 1000ull;
    int retries = 0;

    while (!g_x.done && arch_now() < overall_dl) {
        hal_net_pump();
        sys_check_timeouts();

        if (arch_now() >= retx_at) {
            if (g_x.progress) {
                g_x.progress = false;        /* moving; just reset the timer */
            } else if (retries++ < TFTP_MAX_RETX) {
                if (g_x.block == 0) send_rrq(server, filename);
                else                send_ack(server, g_x.srv_port, g_x.block);
            } else {
                break;
            }
            retx_at = arch_now() + (hz * TFTP_RETX_MS) / 1000ull;
        }
        arch_relax();
    }

    udp_remove(g_pcb);
    g_pcb = NULL;

    if (g_x.done && !g_x.error) {
        if (out_len) *out_len = g_x.len;
        return 0;
    }
    return -1;
}
