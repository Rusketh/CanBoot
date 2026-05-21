#ifndef CANBOOT_LWIP_BIO_H
#define CANBOOT_LWIP_BIO_H

#include <stddef.h>
#include <stdint.h>

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"

/*
 * Synchronous TCP wrapper around lwIP's raw API for use as the
 * mbedtls_ssl_send/recv transport. NO_SYS=1 cooperative model: every
 * blocking operation pumps the net stack until progress is possible.
 */

#define CANBOOT_LWIP_BIO_RX_SIZE 16384u

struct canboot_lwip_bio {
    struct tcp_pcb *pcb;
    unsigned char   rx_buf[CANBOOT_LWIP_BIO_RX_SIZE];
    size_t          rx_head;     /* monotonically increasing write index */
    size_t          rx_tail;     /* monotonically increasing read  index */
    int             connected;   /* 0 pending, 1 ok, -1 failed */
    int             closed;
    int             last_err;
};

void canboot_lwip_bio_init(struct canboot_lwip_bio *bio);

/* Initiate a TCP connect; returns 0 on success or -1 on error/timeout. */
int  canboot_lwip_bio_connect(struct canboot_lwip_bio *bio,
                              const ip_addr_t *addr, uint16_t port,
                              uint32_t timeout_ms);

/* mbedtls_ssl_send_t / recv_t signatures. */
int  canboot_lwip_bio_send(void *ctx, const unsigned char *buf, size_t len);
int  canboot_lwip_bio_recv(void *ctx, unsigned char *buf, size_t len);

void canboot_lwip_bio_close(struct canboot_lwip_bio *bio);

#endif /* CANBOOT_LWIP_BIO_H */
