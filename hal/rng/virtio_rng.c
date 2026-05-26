/*
 * virtio-rng (modern virtio-pci, device id 0x1044) wired to the HAL RNG
 * surface. A single request virtqueue: publish a device-writable buffer,
 * kick, wait for the used ring, and copy out however many random bytes the
 * host wrote. Polled and synchronous, matching the cooperative runtime;
 * one request is outstanding at a time so descriptor 0 is reused.
 *
 * This is a supplementary entropy source - the Mbed TLS port mixes it into
 * the CPU sources when present (see net/mbedtls_port/entropy.c) and ignores
 * it when absent, so it never weakens or gates DRBG seeding.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hal/rng.h"
#include "hal/virtio.h"
#include "sync/cpu.h"

#define VIRTIO_PCI_RNG_MODERN       0x1044u  /* non-transitional */
#define VIRTIO_PCI_RNG_TRANSITIONAL 0x1005u  /* legacy + modern caps */
#define RQ_IDX 0u

static struct canboot_virtio_dev g_dev;
static struct canboot_virtq      g_rq;
static __attribute__((aligned(16))) struct canboot_virtq_desc  g_desc[CANBOOT_VIRTQ_SIZE];
static __attribute__((aligned(16))) struct canboot_virtq_avail g_avail;
static __attribute__((aligned(16))) struct canboot_virtq_used  g_used;
static __attribute__((aligned(64))) uint8_t g_buf[64];

static bool g_present;
static bool g_inited;

bool canboot_rng_init(void) {
    if (g_inited) return g_present;
    g_inited = true;

    if (!canboot_virtio_find(VIRTIO_PCI_RNG_MODERN, &g_dev) &&
        !canboot_virtio_find(VIRTIO_PCI_RNG_TRANSITIONAL, &g_dev)) return false;
    if (!canboot_virtio_negotiate(&g_dev, 0)) return false;
    if (g_dev.num_queues < 1) return false;
    if (!canboot_virtio_queue_setup(&g_dev, RQ_IDX, &g_rq,
                                    g_desc, &g_avail, &g_used)) return false;
    if (!canboot_virtio_run(&g_dev)) return false;

    g_present = true;
    return true;
}

bool canboot_rng_present(void) { return g_present; }

int canboot_rng_read(void *out, uint32_t len) {
    if (!g_inited) canboot_rng_init();
    if (!g_present || !out) return -1;

    uint8_t *o = out;
    uint32_t got = 0;
    while (got < len) {
        uint32_t chunk = len - got;
        if (chunk > sizeof(g_buf)) chunk = sizeof(g_buf);

        canboot_virtq_publish_writable(&g_rq, 0, g_buf, chunk);
        canboot_virtq_kick(&g_rq, RQ_IDX);

        uint16_t done = 0;
        for (uint64_t guard = 0; guard < 50000000ull; guard++) {
            done = canboot_virtq_used_advance(&g_rq);
            if (done) break;
            canboot_cpu_relax();
        }
        if (done == 0) return (int)got;       /* transport stalled */

        uint16_t slot = (uint16_t)(g_rq.last_used_idx % g_rq.size);
        uint32_t fill = g_rq.used->ring[slot].len;
        g_rq.last_used_idx = (uint16_t)(g_rq.last_used_idx + done);
        if (fill > chunk) fill = chunk;
        if (fill == 0) return (int)got;

        memcpy(o + got, g_buf, fill);
        got += fill;
    }
    return (int)got;
}
