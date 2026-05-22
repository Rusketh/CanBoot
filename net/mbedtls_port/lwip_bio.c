/*
 * lwIP raw-TCP <-> Mbed TLS BIO adapter. Mbed TLS's TLS state machine
 * calls send/recv synchronously; our cooperative single-threaded model
 * means each call pumps the net stack until it can make progress.
 */

#include <string.h>

#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "mbedtls/ssl.h"

#include "hal/net.h"
#include "lwip_bio.h"

extern uint64_t canboot_tsc_hz(void);

static inline uint64_t rdtsc_now(void) {
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

static void pump_once(void) {
    hal_net_pump();
    sys_check_timeouts();
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#endif
}

static err_t connect_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    struct canboot_lwip_bio *bio = arg;
    if (err == ERR_OK) {
        bio->connected = 1;
    } else {
        bio->connected = -1;
        bio->last_err  = err;
    }
    return ERR_OK;
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct canboot_lwip_bio *bio = arg;
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        bio->closed = 1;
        bio->last_err = err;
        return ERR_OK;
    }
    if (!p) {
        bio->closed = 1;
        return ERR_OK;
    }

    /* Copy pbuf chain into the circular buffer. Drop overflow rather
     * than block; Mbed TLS reads quickly enough in practice. */
    size_t avail = CANBOOT_LWIP_BIO_RX_SIZE - (bio->rx_head - bio->rx_tail);
    size_t to_copy = p->tot_len;
    if (to_copy > avail) to_copy = avail;

    size_t copied = 0;
    while (copied < to_copy) {
        size_t head_pos = (bio->rx_head + copied) % CANBOOT_LWIP_BIO_RX_SIZE;
        size_t contig   = CANBOOT_LWIP_BIO_RX_SIZE - head_pos;
        size_t chunk    = to_copy - copied;
        if (chunk > contig) chunk = contig;
        pbuf_copy_partial(p, bio->rx_buf + head_pos, chunk, copied);
        copied += chunk;
    }
    bio->rx_head += copied;

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err) {
    struct canboot_lwip_bio *bio = arg;
    bio->closed   = 1;
    bio->last_err = err;
    bio->pcb      = NULL;   /* lwIP has already freed the pcb */
}

void canboot_lwip_bio_init(struct canboot_lwip_bio *bio) {
    memset(bio, 0, sizeof(*bio));
}

int canboot_lwip_bio_connect(struct canboot_lwip_bio *bio,
                             const ip_addr_t *addr, uint16_t port,
                             uint32_t timeout_ms) {
    bio->pcb = tcp_new();
    if (!bio->pcb) return -1;

    tcp_arg (bio->pcb, bio);
    tcp_recv(bio->pcb, recv_cb);
    tcp_err (bio->pcb, err_cb);

    err_t e = tcp_connect(bio->pcb, addr, port, connect_cb);
    if (e != ERR_OK) {
        tcp_abort(bio->pcb);
        bio->pcb = NULL;
        return -1;
    }

    uint64_t hz = canboot_tsc_hz();
    uint64_t dl = rdtsc_now() + (hz * timeout_ms) / 1000ull;
    while (bio->connected == 0 && !bio->closed && rdtsc_now() < dl) {
        pump_once();
    }
    if (bio->connected != 1) {
        if (bio->pcb) {
            tcp_abort(bio->pcb);
            bio->pcb = NULL;
        }
        return -1;
    }
    return 0;
}

static uint32_t bio_stat_send_calls;
static uint32_t bio_stat_send_bytes;
static uint32_t bio_stat_recv_calls;
static uint32_t bio_stat_recv_bytes;

uint32_t canboot_lwip_bio_send_calls(void) { return bio_stat_send_calls; }
uint32_t canboot_lwip_bio_send_bytes(void) { return bio_stat_send_bytes; }
uint32_t canboot_lwip_bio_recv_calls(void) { return bio_stat_recv_calls; }
uint32_t canboot_lwip_bio_recv_bytes(void) { return bio_stat_recv_bytes; }

int canboot_lwip_bio_send(void *vctx, const unsigned char *buf, size_t len) {
    struct canboot_lwip_bio *bio = vctx;
    bio_stat_send_calls++;
    if (!bio->pcb || bio->closed) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }

    size_t total = 0;
    while (total < len) {
        u16_t avail = tcp_sndbuf(bio->pcb);
        if (avail == 0) {
            tcp_output(bio->pcb);
            for (int i = 0; i < 5000; i++) {
                pump_once();
                if (!bio->pcb || bio->closed) break;
                if (tcp_sndbuf(bio->pcb) > 0) break;
            }
            if (!bio->pcb || bio->closed) {
                return MBEDTLS_ERR_SSL_CONN_EOF;
            }
            continue;
        }
        size_t chunk = len - total;
        if (chunk > avail) chunk = avail;
        err_t e = tcp_write(bio->pcb, buf + total, (u16_t)chunk,
                            TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM) {
            tcp_output(bio->pcb);
            pump_once();
            continue;
        }
        if (e != ERR_OK) {
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }
        total += chunk;
    }
    tcp_output(bio->pcb);
    bio_stat_send_bytes += (uint32_t)total;
    return (int)total;
}

int canboot_lwip_bio_recv(void *vctx, unsigned char *buf, size_t len) {
    struct canboot_lwip_bio *bio = vctx;
    bio_stat_recv_calls++;

    /* Pump once each call so the handshake loop above us gets a chance
     * to log progress. If there's still nothing to read after the pump
     * and we haven't been told the peer closed, hand control back to
     * Mbed TLS with WANT_READ so it can retry. */
    if (bio->rx_head == bio->rx_tail) {
        pump_once();
    }
    if (bio->rx_head == bio->rx_tail) {
        if (bio->closed) {
            return MBEDTLS_ERR_SSL_CONN_EOF;
        }
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    size_t avail   = bio->rx_head - bio->rx_tail;
    size_t to_copy = avail < len ? avail : len;
    size_t tail_pos = bio->rx_tail % CANBOOT_LWIP_BIO_RX_SIZE;

    if (tail_pos + to_copy <= CANBOOT_LWIP_BIO_RX_SIZE) {
        memcpy(buf, bio->rx_buf + tail_pos, to_copy);
    } else {
        size_t first = CANBOOT_LWIP_BIO_RX_SIZE - tail_pos;
        memcpy(buf,         bio->rx_buf + tail_pos, first);
        memcpy(buf + first, bio->rx_buf,           to_copy - first);
    }
    bio->rx_tail += to_copy;
    bio_stat_recv_bytes += (uint32_t)to_copy;
    return (int)to_copy;
}

void canboot_lwip_bio_close(struct canboot_lwip_bio *bio) {
    if (bio->pcb) {
        tcp_arg (bio->pcb, NULL);
        tcp_recv(bio->pcb, NULL);
        tcp_err (bio->pcb, NULL);
        tcp_close(bio->pcb);
        bio->pcb = NULL;
    }
}
