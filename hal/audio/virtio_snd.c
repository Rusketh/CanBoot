/*
 * virtio-sound driver for aarch64 (and any virt machine where the
 * sound device is a virtio PCI function rather than HDA). Polled,
 * single output stream, fixed 44.1 kHz stereo signed-16 PCM to
 * match the rest of canboot's audio HAL surface.
 *
 * Layout matches the rest of the virtio drivers in canboot:
 *  - Find the device via canboot_virtio_find (PCI vendor 0x1AF4,
 *    modern device id 0x1059).
 *  - Negotiate with no extra driver features (basic playback is
 *    in the legacy device feature set).
 *  - Bring up two virtqueues: controlq (idx 0) and txq (idx 2).
 *    eventq + rxq stay un-set-up - we don't consume device events
 *    and never capture audio.
 *  - On the control queue: enumerate output streams, issue
 *    SET_PARAMS, PREPARE and START on the first one.
 *  - On the tx queue: each hal_audio_write call lays down a
 *    3-descriptor chain (hdr -> PCM data -> status response) and
 *    blocks until the device acks.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "hal/audio.h"
#include "hal/virtio.h"

#define VIRTIO_PCI_SOUND 0x1059u

/* Command codes (virtio-snd 1.2). */
#define VIRTIO_SND_R_PCM_INFO       0x0100u
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101u
#define VIRTIO_SND_R_PCM_PREPARE    0x0102u
#define VIRTIO_SND_R_PCM_RELEASE    0x0103u
#define VIRTIO_SND_R_PCM_START      0x0104u
#define VIRTIO_SND_R_PCM_STOP       0x0105u

/* Status codes. */
#define VIRTIO_SND_S_OK 0x8000u

/* PCM format / rate. */
#define VIRTIO_SND_PCM_FMT_S16  5u
#define VIRTIO_SND_PCM_RATE_44100 6u

/* Stream direction. */
#define VIRTIO_SND_D_OUTPUT 0u

/* virtio-snd config layout (4.x device-specific config). */
struct __attribute__((packed)) virtio_snd_config {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
};

struct __attribute__((packed)) virtio_snd_hdr {
    uint32_t code;
};

struct __attribute__((packed)) virtio_snd_pcm_hdr {
    struct virtio_snd_hdr hdr;
    uint32_t              stream_id;
};

struct __attribute__((packed)) virtio_snd_query_info {
    struct virtio_snd_hdr hdr;
    uint32_t              start_id;
    uint32_t              count;
    uint32_t              size;
};

struct __attribute__((packed)) virtio_snd_pcm_set_params {
    struct virtio_snd_pcm_hdr hdr;
    uint32_t                  buffer_bytes;
    uint32_t                  period_bytes;
    uint32_t                  features;
    uint8_t                   channels;
    uint8_t                   format;
    uint8_t                   rate;
    uint8_t                   padding;
};

struct __attribute__((packed)) virtio_snd_pcm_info {
    struct virtio_snd_hdr hdr;
    uint32_t              features;
    uint64_t              formats;
    uint64_t              rates;
    uint8_t               direction;
    uint8_t               channels_min;
    uint8_t               channels_max;
    uint8_t               padding[5];
};

struct __attribute__((packed)) virtio_snd_pcm_xfer {
    uint32_t stream_id;
};

struct __attribute__((packed)) virtio_snd_pcm_status {
    uint32_t status;
    uint32_t latency_bytes;
};

/* Sizes / counts. */
#define VSND_MAX_PCM_INFOS 8u
#define VSND_TX_SLOTS      8u
#define VSND_PCM_BUF_BYTES (16u * 1024u)  /* per TX slot */

/* Queue indices defined by the spec. */
#define QIDX_CONTROL 0u
#define QIDX_TX      2u

static __attribute__((aligned(16)))
struct canboot_virtq_desc  ctrl_desc[CANBOOT_VIRTQ_SIZE];
static __attribute__((aligned(2)))
struct canboot_virtq_avail ctrl_avail;
static __attribute__((aligned(4)))
struct canboot_virtq_used  ctrl_used;

static __attribute__((aligned(16)))
struct canboot_virtq_desc  tx_desc[CANBOOT_VIRTQ_SIZE];
static __attribute__((aligned(2)))
struct canboot_virtq_avail tx_avail;
static __attribute__((aligned(4)))
struct canboot_virtq_used  tx_used;

/* Per-control-message scratch. Big enough for the biggest query the
 * driver issues (PCM_INFO list = MAX_INFOS * sizeof(pcm_info)). */
static __attribute__((aligned(8))) uint8_t ctrl_cmd_buf[1024];
static __attribute__((aligned(8))) uint8_t ctrl_rsp_buf[1024];

/* TX pre-allocated buffers. Each slot carries:
 *   [0]   virtio_snd_pcm_xfer    (driver->device, readable)
 *   [1]   PCM data (driver->device, readable)
 *   [2]   virtio_snd_pcm_status  (device->driver, writable)
 *
 * Three descriptors per slot, descriptors 3*N + 0, 3*N + 1, 3*N + 2. */
static __attribute__((aligned(16)))
struct virtio_snd_pcm_xfer    tx_hdrs[VSND_TX_SLOTS];
static __attribute__((aligned(16)))
uint8_t                        tx_data[VSND_TX_SLOTS][VSND_PCM_BUF_BYTES];
static __attribute__((aligned(16)))
struct virtio_snd_pcm_status   tx_status[VSND_TX_SLOTS];

struct vsnd_state {
    bool                       present;
    struct canboot_virtio_dev  dev;
    struct canboot_virtq       ctrlq;
    struct canboot_virtq       txq;
    uint32_t                   n_streams;
    uint32_t                   out_stream_id;
    uint32_t                   tx_next;  /* next free TX slot */
    char                       dev_name[20];
};
static struct vsnd_state g_vsnd;

static void busy_wait(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) {
        __asm__ volatile ("nop");
    }
}

/* Send one control command and wait for the response. Returns the
 * 32-bit status word from virtio_snd_hdr.code (VIRTIO_SND_S_OK on
 * success, error code otherwise). */
static uint32_t vsnd_ctrl(const void *cmd, uint32_t cmd_len,
                          void *rsp, uint32_t rsp_len) {
    if (cmd_len > sizeof(ctrl_cmd_buf)) return 0xFFFFFFFFu;
    if (rsp_len > sizeof(ctrl_rsp_buf)) return 0xFFFFFFFFu;
    memcpy(ctrl_cmd_buf, cmd, cmd_len);

    /* Two-descriptor chain: cmd (R) -> response (W). */
    ctrl_desc[0].addr  = (uint64_t)(uintptr_t)ctrl_cmd_buf;
    ctrl_desc[0].len   = cmd_len;
    ctrl_desc[0].flags = CANBOOT_VIRTQ_DESC_F_NEXT;
    ctrl_desc[0].next  = 1;
    ctrl_desc[1].addr  = (uint64_t)(uintptr_t)ctrl_rsp_buf;
    ctrl_desc[1].len   = rsp_len;
    ctrl_desc[1].flags = CANBOOT_VIRTQ_DESC_F_WRITE;
    ctrl_desc[1].next  = 0;

    /* Publish desc 0 on avail ring and kick. */
    uint16_t avail_idx = g_vsnd.ctrlq.avail->idx;
    g_vsnd.ctrlq.avail->ring[avail_idx % g_vsnd.ctrlq.size] = 0;
    __asm__ volatile ("" ::: "memory");
    g_vsnd.ctrlq.avail->idx = (uint16_t)(avail_idx + 1);
    canboot_virtq_kick(&g_vsnd.ctrlq, QIDX_CONTROL);

    /* Wait for the used ring to advance. */
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (g_vsnd.ctrlq.used->idx != g_vsnd.ctrlq.last_used_idx) {
            g_vsnd.ctrlq.last_used_idx++;
            if (rsp && rsp_len) memcpy(rsp, ctrl_rsp_buf, rsp_len);
            const struct virtio_snd_hdr *h = (const struct virtio_snd_hdr *)ctrl_rsp_buf;
            return h->code;
        }
        busy_wait(50);
    }
    return 0xFFFFFFFFu;
}

static bool find_output_stream(void) {
    /* Query PCM infos for all streams; pick the first OUTPUT one. */
    struct virtio_snd_query_info q = {
        .hdr       = { .code = VIRTIO_SND_R_PCM_INFO },
        .start_id  = 0,
        .count     = g_vsnd.n_streams,
        .size      = (uint32_t)sizeof(struct virtio_snd_pcm_info),
    };
    if (q.count == 0 || q.count > VSND_MAX_PCM_INFOS) return false;
    uint32_t rsp_size = (uint32_t)sizeof(struct virtio_snd_hdr)
                      + q.count * (uint32_t)sizeof(struct virtio_snd_pcm_info);
    if (rsp_size > sizeof(ctrl_rsp_buf)) return false;

    uint32_t rc = vsnd_ctrl(&q, (uint32_t)sizeof(q), NULL, rsp_size);
    if (rc != VIRTIO_SND_S_OK) return false;

    const struct virtio_snd_pcm_info *infos =
        (const struct virtio_snd_pcm_info *)
        (ctrl_rsp_buf + sizeof(struct virtio_snd_hdr));
    for (uint32_t i = 0; i < q.count; i++) {
        if (infos[i].direction == VIRTIO_SND_D_OUTPUT) {
            g_vsnd.out_stream_id = i;
            return true;
        }
    }
    return false;
}

static bool configure_stream(void) {
    struct virtio_snd_pcm_set_params p = {0};
    p.hdr.hdr.code     = VIRTIO_SND_R_PCM_SET_PARAMS;
    p.hdr.stream_id    = g_vsnd.out_stream_id;
    p.buffer_bytes     = VSND_PCM_BUF_BYTES * VSND_TX_SLOTS;
    p.period_bytes     = VSND_PCM_BUF_BYTES;
    p.features         = 0;
    p.channels         = (uint8_t)HAL_AUDIO_CHANNELS;
    p.format           = VIRTIO_SND_PCM_FMT_S16;
    p.rate             = VIRTIO_SND_PCM_RATE_44100;
    if (vsnd_ctrl(&p, (uint32_t)sizeof(p), NULL,
                  (uint32_t)sizeof(struct virtio_snd_hdr)) != VIRTIO_SND_S_OK) {
        return false;
    }

    struct virtio_snd_pcm_hdr prep = {
        .hdr       = { .code = VIRTIO_SND_R_PCM_PREPARE },
        .stream_id = g_vsnd.out_stream_id,
    };
    if (vsnd_ctrl(&prep, (uint32_t)sizeof(prep), NULL,
                  (uint32_t)sizeof(struct virtio_snd_hdr)) != VIRTIO_SND_S_OK) {
        return false;
    }

    struct virtio_snd_pcm_hdr start = {
        .hdr       = { .code = VIRTIO_SND_R_PCM_START },
        .stream_id = g_vsnd.out_stream_id,
    };
    return vsnd_ctrl(&start, (uint32_t)sizeof(start), NULL,
                     (uint32_t)sizeof(struct virtio_snd_hdr)) == VIRTIO_SND_S_OK;
}

bool hal_audio_init(void) {
    if (g_vsnd.present) return true;
    if (!canboot_virtio_find(VIRTIO_PCI_SOUND, &g_vsnd.dev)) return false;
    if (!canboot_virtio_negotiate(&g_vsnd.dev, 0)) return false;

    if (!canboot_virtio_queue_setup(&g_vsnd.dev, QIDX_CONTROL,
                                     &g_vsnd.ctrlq,
                                     ctrl_desc, &ctrl_avail, &ctrl_used)) {
        return false;
    }
    if (!canboot_virtio_queue_setup(&g_vsnd.dev, QIDX_TX,
                                     &g_vsnd.txq,
                                     tx_desc, &tx_avail, &tx_used)) {
        return false;
    }
    if (!canboot_virtio_run(&g_vsnd.dev)) return false;

    /* Read device config: streams count. */
    volatile const struct virtio_snd_config *cfg =
        (volatile const struct virtio_snd_config *)g_vsnd.dev.device_cfg;
    g_vsnd.n_streams = cfg->streams;
    if (g_vsnd.n_streams == 0 || g_vsnd.n_streams > VSND_MAX_PCM_INFOS) return false;

    if (!find_output_stream()) return false;
    if (!configure_stream())   return false;

    snprintf(g_vsnd.dev_name, sizeof(g_vsnd.dev_name), "virtio-snd");
    g_vsnd.present = true;
    return true;
}

bool hal_audio_present(void)            { return g_vsnd.present; }
const char *hal_audio_device_name(void) { return g_vsnd.present ? g_vsnd.dev_name : "none"; }

/* Push samples as a 3-descriptor chain. Returns the number of frames
 * actually accepted (may be less than `frames` when the ring is full
 * or the payload exceeds VSND_PCM_BUF_BYTES). */
uint32_t hal_audio_write(const int16_t *samples, uint32_t frames) {
    if (!g_vsnd.present) return frames;
    if (!samples || frames == 0) return 0;
    uint32_t bytes_per_frame = HAL_AUDIO_CHANNELS * HAL_AUDIO_BPS;
    uint32_t bytes_total = frames * bytes_per_frame;
    if (bytes_total > VSND_PCM_BUF_BYTES) {
        bytes_total = VSND_PCM_BUF_BYTES;
        frames = bytes_total / bytes_per_frame;
    }

    /* Wait for a free TX slot. */
    uint16_t slot = (uint16_t)(g_vsnd.tx_next % VSND_TX_SLOTS);
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint16_t used_idx = g_vsnd.txq.used->idx;
        uint16_t outstanding =
            (uint16_t)(g_vsnd.tx_next - g_vsnd.txq.last_used_idx);
        if (outstanding < VSND_TX_SLOTS) break;
        if (used_idx != g_vsnd.txq.last_used_idx) {
            g_vsnd.txq.last_used_idx++;
        }
        busy_wait(50);
    }

    /* Build the chain: header (R) -> data (R) -> status (W). */
    uint16_t base = (uint16_t)(slot * 3u);
    tx_hdrs[slot].stream_id = g_vsnd.out_stream_id;
    memcpy(tx_data[slot], samples, bytes_total);
    g_vsnd.txq.desc[base + 0].addr  = (uint64_t)(uintptr_t)&tx_hdrs[slot];
    g_vsnd.txq.desc[base + 0].len   = (uint32_t)sizeof(struct virtio_snd_pcm_xfer);
    g_vsnd.txq.desc[base + 0].flags = CANBOOT_VIRTQ_DESC_F_NEXT;
    g_vsnd.txq.desc[base + 0].next  = (uint16_t)(base + 1);
    g_vsnd.txq.desc[base + 1].addr  = (uint64_t)(uintptr_t)tx_data[slot];
    g_vsnd.txq.desc[base + 1].len   = bytes_total;
    g_vsnd.txq.desc[base + 1].flags = CANBOOT_VIRTQ_DESC_F_NEXT;
    g_vsnd.txq.desc[base + 1].next  = (uint16_t)(base + 2);
    g_vsnd.txq.desc[base + 2].addr  = (uint64_t)(uintptr_t)&tx_status[slot];
    g_vsnd.txq.desc[base + 2].len   = (uint32_t)sizeof(struct virtio_snd_pcm_status);
    g_vsnd.txq.desc[base + 2].flags = CANBOOT_VIRTQ_DESC_F_WRITE;
    g_vsnd.txq.desc[base + 2].next  = 0;

    uint16_t avail_idx = g_vsnd.txq.avail->idx;
    g_vsnd.txq.avail->ring[avail_idx % g_vsnd.txq.size] = base;
    __asm__ volatile ("" ::: "memory");
    g_vsnd.txq.avail->idx = (uint16_t)(avail_idx + 1);
    canboot_virtq_kick(&g_vsnd.txq, QIDX_TX);

    g_vsnd.tx_next++;
    return frames;
}

void hal_audio_flush(void) {
    if (!g_vsnd.present) return;
    /* Drain all pending TX slots. Bounded poll so a wedged device
     * doesn't block boot. */
    for (uint32_t i = 0; i < 2000000u; i++) {
        uint16_t outstanding =
            (uint16_t)(g_vsnd.tx_next - g_vsnd.txq.last_used_idx);
        if (outstanding == 0) return;
        if (g_vsnd.txq.used->idx != g_vsnd.txq.last_used_idx) {
            g_vsnd.txq.last_used_idx++;
        }
        busy_wait(50);
    }
}

void hal_audio_stop(void) {
    if (!g_vsnd.present) return;
    struct virtio_snd_pcm_hdr stop = {
        .hdr       = { .code = VIRTIO_SND_R_PCM_STOP },
        .stream_id = g_vsnd.out_stream_id,
    };
    vsnd_ctrl(&stop, (uint32_t)sizeof(stop), NULL,
              (uint32_t)sizeof(struct virtio_snd_hdr));
}
