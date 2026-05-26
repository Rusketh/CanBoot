/*
 * Polled Intel HDA driver. Targets QEMU's ICH9 HDA (vendor 0x8086,
 * device 0x293E) with the standard `hda-output` codec, plus any
 * PCI device that advertises class 0x04 / subclass 0x03 (HDA).
 * IRQs aren't used; we poll SDx_LPIB to track the DMA position and
 * fill the ring on demand. That trades latency for simplicity -
 * the cando audio library calls hal_audio_write in bursts which is
 * a good match for a 32 KiB ring.
 *
 * Layout:
 *  - 32 KiB ring split into two 16 KiB halves
 *  - 2-entry BDL pointing at each half
 *  - 1 output stream (stream tag 1) configured for 44.1 kHz stereo s16
 *
 * Codec setup is deliberately bare. We:
 *  1. CORB/RIRB ring up + GCTL reset
 *  2. STATESTS scan -> first non-zero codec address
 *  3. Walk root node -> first audio function group (AFG)
 *  4. Walk the AFG -> first audio output converter widget
 *  5. Pick the first output pin connected to that converter
 *  6. Set converter stream + format, enable pin amp + EAPD + power.
 *
 * That covers the QEMU codec topology in one pass. Real hardware
 * with multiple AFGs or pin selectors needs a fuller walk but the
 * boot-chime use-case is fine with the simple path.
 */

#if defined(__x86_64__)

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "hal/audio.h"
#include "hal/pci.h"
#include "audio_x86.h"

#define HDA_GCAP      0x00
#define HDA_GCTL      0x08
#define HDA_WAKEEN    0x0C
#define HDA_STATESTS  0x0E
#define HDA_INTCTL    0x20
#define HDA_INTSTS    0x24
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP    0x48
#define HDA_CORBRP    0x4A
#define HDA_CORBCTL   0x4C
#define HDA_CORBSIZE  0x4E
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP    0x58
#define HDA_RIRBCTL   0x5C
#define HDA_RIRBSIZE  0x5E
#define HDA_DPLBASE   0x70  /* DMA position lower */
#define HDA_DPUBASE   0x74  /* DMA position upper */

/* Per-stream descriptor offsets. ICH9 has 4 input + 4 output streams;
 * output 0 lives at base + 0x100 (in 0..3 at 0x80..0xE0). */
#define HDA_NIN_DEFAULT  4u
#define HDA_SD_BASE      0x80
#define HDA_SD_STRIDE    0x20
#define SD_CTL    0x00
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FIFOS  0x10
#define SD_FMT    0x12
#define SD_BDPL   0x18
#define SD_BDPU   0x1C

/* SD_FMT bits: rate 44.1k stereo 16-bit = base 44.1k, mult 1, div 1,
 * bits 16 (001b), channels 2 (chan-1 = 0001). 16-bit field:
 *  [15] = 1 for 44.1k base, 0 for 48k
 *  [14:11] BASE mult, [10:8] DIV
 *  [7] = 0  (reserved)
 *  [6:4] bits-per-sample (000=8, 001=16, 010=20, 011=24, 100=32)
 *  [3:0] channels - 1 */
#define HDA_FMT_44K1_STEREO_S16 ((1u << 14) | (1u << 4) | 0x1u)

#define HDA_STREAM_TAG 1u

/* Codec command bus. CORB writes verbs, RIRB reads responses.
 * Verb encoding: [31:28]=cad, [27:20]=nid, [19:0]=payload.
 *
 * Common verbs:
 *  GET_PARAMETER       0xF00 payload=param-id
 *  CONNECTION_LIST     0xF02 payload=index
 *  STREAM/CHAN         0x706 payload=(tag<<4 | channel)
 *  SET_FORMAT          0x200 (verb-type) ... actually format is a
 *                      16-bit verb (0x2 verb high); easier to use
 *                      the long form 0x70_02_xxxx via SET_CONVERTER.
 *  POWER_STATE         0x705
 *  PIN_WIDGET_CONTROL  0x707
 *  AMP_GAIN_MUTE       0x300 long-form / 0x3 short-form
 *  EAPD_BTL_ENABLE     0x70C
 */
static inline uint32_t mk_verb(uint8_t cad, uint16_t nid, uint16_t verb,
                               uint16_t payload) {
    return ((uint32_t)(cad & 0xF) << 28)
         | ((uint32_t)nid << 20)
         | ((uint32_t)(verb & 0xFFF) << 8)
         | (uint32_t)(payload & 0xFF);
}
/* Long-form verb (16-bit verb code + 16-bit payload) for SET_CONVERTER_FORMAT etc. */
static inline uint32_t mk_verb_long(uint8_t cad, uint16_t nid, uint8_t verb,
                                     uint16_t payload) {
    return ((uint32_t)(cad & 0xF) << 28)
         | ((uint32_t)nid << 20)
         | ((uint32_t)(verb & 0xF)  << 16)
         | (uint32_t)payload;
}

/* Pre-allocated DMA-able areas. 128-byte alignment satisfies the
 * controller's BDL alignment requirement. Identity-mapped on
 * canboot x86_64 so physical == virtual. */
static __attribute__((aligned(128))) uint8_t  g_bdl[256];    /* room for 16 entries */
static __attribute__((aligned(128))) uint8_t  g_pcm_ring[32 * 1024];

#define BDL_ENTRIES  2u
#define RING_BYTES   sizeof(g_pcm_ring)
#define HALF_BYTES   (RING_BYTES / 2u)

struct hda_state {
    bool                          present;
    volatile uint8_t             *regs;
    uint8_t                       n_in_streams;
    uint16_t                      out_sd_off;  /* offset of stream-descriptor 0 (first out) */
    uint8_t                       codec_addr;
    uint16_t                      converter_nid;
    uint16_t                      pin_nid;
    uint32_t                      write_pos;   /* host write cursor into g_pcm_ring */
    char                          dev_name[24];
};
static struct hda_state g_hda;

/* MMIO accessors. The HDA spec requires 32-bit access for most regs;
 * STATESTS and SD_STS are 16/8 bit. */
static inline uint32_t r32(uint32_t off) {
    return *(volatile uint32_t *)(g_hda.regs + off);
}
static inline void w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_hda.regs + off) = v;
}
static inline uint16_t r16(uint32_t off) {
    return *(volatile uint16_t *)(g_hda.regs + off);
}
static inline void w16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(g_hda.regs + off) = v;
}
static inline uint8_t r8(uint32_t off) {
    return *(volatile uint8_t *)(g_hda.regs + off);
}
static inline void w8(uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(g_hda.regs + off) = v;
}

static void busy_wait(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) __asm__ volatile ("pause");
}

/* Immediate Command Output / Input / Status registers - the legacy
 * non-DMA path for sending a single verb at a time. QEMU's HDA
 * controller implements ICOI/ICII/ICIS and is much easier to drive
 * than the CORB/RIRB ring during one-shot codec discovery. */
#define HDA_ICOI 0x60
#define HDA_ICII 0x64
#define HDA_ICIS 0x68
#define ICIS_ICB (1u << 0)  /* Immediate Command Busy */
#define ICIS_IRV (1u << 1)  /* Immediate Result Valid (W1C) */

/* Send a single verb and wait for the response. Returns 0xFFFFFFFF
 * on timeout (typical for non-existent NIDs). */
static uint32_t hda_cmd(uint32_t verb) {
    /* Wait until the controller is idle. */
    for (int i = 0; i < 10000; i++) {
        if ((r16(HDA_ICIS) & ICIS_ICB) == 0) break;
        busy_wait(50);
    }
    /* Clear any stale IRV before issuing. */
    w16(HDA_ICIS, ICIS_IRV);
    /* Push the verb and start the transaction. */
    w32(HDA_ICOI, verb);
    w16(HDA_ICIS, ICIS_ICB);
    /* Wait for the result. */
    for (int i = 0; i < 10000; i++) {
        uint16_t st = r16(HDA_ICIS);
        if (st & ICIS_IRV) {
            uint32_t resp = r32(HDA_ICII);
            w16(HDA_ICIS, ICIS_IRV); /* W1C */
            return resp;
        }
        busy_wait(50);
    }
    return 0xFFFFFFFFu;
}

static bool reset_controller(void) {
    /* Drop CRST bit, wait for 0, then raise and wait for 1. */
    uint32_t gctl = r32(HDA_GCTL);
    w32(HDA_GCTL, gctl & ~1u);
    for (int i = 0; i < 100; i++) {
        if ((r32(HDA_GCTL) & 1u) == 0) break;
        busy_wait(1000);
    }
    w32(HDA_GCTL, gctl | 1u);
    for (int i = 0; i < 1000; i++) {
        if (r32(HDA_GCTL) & 1u) break;
        busy_wait(1000);
    }
    if ((r32(HDA_GCTL) & 1u) == 0) return false;
    busy_wait(50000);  /* 521 us per spec for codec state-change announcements */
    return true;
}

static uint16_t find_audio_function_group(uint8_t cad) {
    /* Root node = 0. GET_PARAMETER NODE_COUNT (param 0x04) returns
     *   [23:16] start_nid, [7:0] total_node_count. */
    uint32_t r = hda_cmd(mk_verb(cad, 0, 0xF00, 0x04));
    uint16_t start = (uint16_t)((r >> 16) & 0xFF);
    uint16_t count = (uint16_t)(r & 0xFF);
    for (uint16_t i = 0; i < count; i++) {
        uint16_t nid = (uint16_t)(start + i);
        /* GET_PARAMETER FUNCTION_GROUP_TYPE (0x05) low byte: 1 = AFG */
        uint32_t fgtype = hda_cmd(mk_verb(cad, nid, 0xF00, 0x05));
        if ((fgtype & 0xFF) == 0x01) return nid;
    }
    return 0;
}

static uint16_t find_audio_output(uint8_t cad, uint16_t afg, uint16_t *out_pin) {
    *out_pin = 0;
    /* SUB_NODE_COUNT under the AFG. */
    uint32_t r = hda_cmd(mk_verb(cad, afg, 0xF00, 0x04));
    uint16_t start = (uint16_t)((r >> 16) & 0xFF);
    uint16_t count = (uint16_t)(r & 0xFF);
    uint16_t converter = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t nid = (uint16_t)(start + i);
        /* GET_PARAMETER AUDIO_WIDGET_CAPABILITIES (0x09).
         * Type at bits [23:20]: 0 = audio output, 4 = pin complex. */
        uint32_t caps = hda_cmd(mk_verb(cad, nid, 0xF00, 0x09));
        uint8_t type = (uint8_t)((caps >> 20) & 0xF);
        if (type == 0x0 && converter == 0) converter = nid;
        if (type == 0x4) {
            /* PIN_CAPABILITIES (0x0C) bit 4 = "output capable" */
            uint32_t pcaps = hda_cmd(mk_verb(cad, nid, 0xF00, 0x0C));
            if (pcaps & (1u << 4) && *out_pin == 0) *out_pin = nid;
        }
    }
    return converter;
}

static void build_bdl(void) {
    /* Two 16 KiB halves of g_pcm_ring. */
    for (uint32_t i = 0; i < BDL_ENTRIES; i++) {
        uint8_t *entry = g_bdl + i * 16;
        uintptr_t addr = (uintptr_t)g_pcm_ring + i * HALF_BYTES;
        uint32_t  len  = HALF_BYTES;
        entry[0]  = (uint8_t)(addr      );
        entry[1]  = (uint8_t)(addr >>  8);
        entry[2]  = (uint8_t)(addr >> 16);
        entry[3]  = (uint8_t)(addr >> 24);
        entry[4]  = (uint8_t)(addr >> 32);
        entry[5]  = (uint8_t)(addr >> 40);
        entry[6]  = (uint8_t)(addr >> 48);
        entry[7]  = (uint8_t)(addr >> 56);
        entry[8]  = (uint8_t)(len       );
        entry[9]  = (uint8_t)(len  >>  8);
        entry[10] = (uint8_t)(len  >> 16);
        entry[11] = (uint8_t)(len  >> 24);
        entry[12] = 0; entry[13] = 0;
        entry[14] = 0; entry[15] = 0; /* IOC = 0 */
    }
}

static void program_stream(void) {
    uint32_t sd = g_hda.out_sd_off;
    /* Stop. */
    w8(sd + SD_CTL, 0);
    busy_wait(1000);
    /* Reset stream: bit 0 = SRST. */
    w8(sd + SD_CTL, 0x1);
    for (int i = 0; i < 100; i++) {
        if (r8(sd + SD_CTL) & 0x1) break;
        busy_wait(200);
    }
    w8(sd + SD_CTL, 0);
    for (int i = 0; i < 100; i++) {
        if ((r8(sd + SD_CTL) & 0x1) == 0) break;
        busy_wait(200);
    }
    /* Buffer descriptor list. */
    uintptr_t bdl_pa = (uintptr_t)g_bdl;
    w32(sd + SD_BDPL, (uint32_t)bdl_pa);
    w32(sd + SD_BDPU, (uint32_t)(bdl_pa >> 32));
    w32(sd + SD_CBL, RING_BYTES);
    w16(sd + SD_LVI, (uint16_t)(BDL_ENTRIES - 1));
    /* 44.1 kHz stereo s16. */
    w16(sd + SD_FMT, HDA_FMT_44K1_STEREO_S16);
    /* Stream tag in bits [23:20] of SD_CTL (which spans 3 bytes). We
     * use a 32-bit access since the spec lays SD_CTL across offsets
     * 0x00..0x02. Bit 1 = RUN. */
    uint32_t ctl = ((uint32_t)HDA_STREAM_TAG << 20) | 0x00040002u;
    w32(sd + SD_CTL, ctl);
}

static void program_codec(uint8_t cad, uint16_t converter, uint16_t pin) {
    /* Power up AFG + widgets (long form verb 0x705 payload 0=D0). */
    hda_cmd(mk_verb(cad, converter, 0x705, 0x00));
    hda_cmd(mk_verb(cad, pin,        0x705, 0x00));
    /* Set converter stream/channel: stream=1, channel=0 (left). */
    hda_cmd(mk_verb(cad, converter, 0x706, (HDA_STREAM_TAG << 4) | 0));
    /* Set converter format = same as SD_FMT. Long form verb 0x2. */
    hda_cmd(mk_verb_long(cad, converter, 0x2, HDA_FMT_44K1_STEREO_S16));
    /* Enable EAPD/BTL on the pin (verb 0x70C, payload bit 1 = EAPD). */
    hda_cmd(mk_verb(cad, pin, 0x70C, 0x02));
    /* Pin widget control: bit 6 = OUT enable, bit 7 = HP enable. */
    hda_cmd(mk_verb(cad, pin, 0x707, 0xC0));
    /* Unmute output amp on the pin + converter at moderately high gain.
     *   AMP_GAIN_MUTE verb 0x3 long-form, payload bits:
     *     [15]=Output, [14]=Input, [13]=Left, [12]=Right,
     *     [7]=Mute, [6:0]=Gain. We set both channels of the output
     *     amp with gain 0x50 (near-max for codecs whose amp caps top
     *     out around 0x7F) and the mute bit clear. */
    hda_cmd(mk_verb_long(cad, converter, 0x3, 0xB050));
    hda_cmd(mk_verb_long(cad, pin,       0x3, 0xB050));
}

static bool probe_pci(struct canboot_pci_addr *out) {
    /* HDA: class 0x04 (Multimedia) subclass 0x03 (Audio device). */
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *devs = hal_pci_devs();
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].class_code == 0x04 && devs[i].subclass == 0x03) {
            *out = devs[i].addr;
            return true;
        }
    }
    return false;
}

bool canboot_hda_init(void) {
    if (g_hda.present) return true;
    struct canboot_pci_addr addr;
    if (!probe_pci(&addr)) return false;

    hal_pci_enable_bus_master(addr);
    uint64_t bar0 = hal_pci_bar_addr(addr, 0);
    if (!bar0) return false;
    g_hda.regs = (volatile uint8_t *)(uintptr_t)bar0;

    if (!reset_controller()) return false;
    /* CORB/RIRB ring setup intentionally skipped - we use the
     * immediate-command path (ICOI/ICII/ICIS) which doesn't need a
     * verb DMA ring. The CORB/RIRB run bits stay 0 and the rings
     * sit unused. */

    /* GCAP at offset 0: [15:8]=ISS (input streams), [7:0] = OSS etc.
     * QEMU ICH9: 4 input streams, 4 output streams. */
    uint16_t gcap = r16(HDA_GCAP);
    uint8_t  iss  = (uint8_t)((gcap >> 8) & 0xF);
    g_hda.n_in_streams = iss ? iss : HDA_NIN_DEFAULT;
    g_hda.out_sd_off = HDA_SD_BASE + g_hda.n_in_streams * HDA_SD_STRIDE;

    /* DMA position write-back left disabled. We poll SDx_LPIB
     * directly in hal_audio_write/flush; the write-back area would
     * have to be sized for every stream (8 streams * 8 bytes on
     * ICH9 = 64 bytes) and points back into our own BSS, which
     * just makes corruption likelier without a real upside for
     * a polled writer. */
    w32(HDA_DPLBASE, 0);
    w32(HDA_DPUBASE, 0);

    /* Find codec. STATESTS bit n set = codec at address n responded. */
    uint16_t st = r16(HDA_STATESTS);
    if (!st) return false;
    for (int i = 0; i < 15; i++) {
        if (st & (1u << i)) { g_hda.codec_addr = (uint8_t)i; break; }
    }
    uint16_t afg = find_audio_function_group(g_hda.codec_addr);
    if (!afg) return false;
    uint16_t pin = 0;
    uint16_t conv = find_audio_output(g_hda.codec_addr, afg, &pin);
    if (!conv || !pin) return false;
    g_hda.converter_nid = conv;
    g_hda.pin_nid       = pin;

    /* Zero the ring + arm the stream. */
    memset(g_pcm_ring, 0, sizeof(g_pcm_ring));
    build_bdl();
    program_codec(g_hda.codec_addr, conv, pin);
    program_stream();

    snprintf(g_hda.dev_name, sizeof(g_hda.dev_name), "intel-hda");
    g_hda.present = true;
    return true;
}

bool canboot_hda_present(void) { return g_hda.present; }

const char *canboot_hda_device_name(void) {
    return g_hda.present ? g_hda.dev_name : "none";
}

uint32_t canboot_hda_write(const int16_t *samples, uint32_t frames) {
    if (!g_hda.present) return frames;
    if (!samples || frames == 0) return 0;
    uint32_t bytes = frames * HAL_AUDIO_CHANNELS * HAL_AUDIO_BPS;
    uint32_t pushed = 0;
    while (pushed < bytes) {
        /* Wait until the device has consumed enough of the ring that
         * we can write another chunk without overrunning the read
         * pointer. SD_LPIB is updated by the controller. */
        uint32_t sd = g_hda.out_sd_off;
        uint32_t lpib = r32(sd + SD_LPIB);
        uint32_t free_bytes = (RING_BYTES + lpib - g_hda.write_pos) % RING_BYTES;
        if (free_bytes <= 64) { /* keep a tiny gap */
            busy_wait(500);
            continue;
        }
        uint32_t chunk = bytes - pushed;
        if (chunk > free_bytes - 64) chunk = free_bytes - 64;
        uint32_t end_of_ring = RING_BYTES - g_hda.write_pos;
        if (chunk > end_of_ring) chunk = end_of_ring;
        memcpy(g_pcm_ring + g_hda.write_pos,
               (const uint8_t *)samples + pushed, chunk);
        g_hda.write_pos = (g_hda.write_pos + chunk) % RING_BYTES;
        pushed += chunk;
    }
    return frames;
}

void canboot_hda_flush(void) {
    if (!g_hda.present) return;
    /* Wait until the device's LPIB catches up to our write pointer
     * (or close enough). Bounded poll so a wedged device doesn't
     * hang the boot path. */
    uint32_t sd = g_hda.out_sd_off;
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint32_t lpib = r32(sd + SD_LPIB);
        uint32_t gap  = (RING_BYTES + g_hda.write_pos - lpib) % RING_BYTES;
        if (gap < 128) return;
        busy_wait(500);
    }
}

void canboot_hda_stop(void) {
    if (!g_hda.present) return;
    uint32_t sd = g_hda.out_sd_off;
    uint32_t ctl = r32(sd + SD_CTL);
    w32(sd + SD_CTL, ctl & ~0x2u);  /* clear RUN bit */
}

#endif /* __x86_64__ */
