/*
 * Polled Intel AC'97 (ICH) audio driver - QEMU's `-device AC97`
 * (8086:2415) and the AC'97 controllers in older PCs and hypervisors.
 * Class 0x04 / subclass 0x01, two I/O-port BARs: NAM (BAR0, the codec
 * mixer) and NABM (BAR1, the bus-master DMA engine).
 *
 * One output stream (PCM Out). A 32-entry buffer descriptor list maps a
 * 32 KiB ring; hal_audio_write fills the next free buffer, bumps LVI, and
 * (re)starts the bus master. The DMA position is polled via CIV/PICB - no
 * interrupts - matching the cooperative runtime. Variable-rate audio is
 * enabled so the codec runs at the HAL's 44.1 kHz; without VRA the part is
 * fixed at 48 kHz and we leave the codec at its default.
 *
 * DMA structures live in identity-mapped BSS (physical == virtual on
 * canboot x86_64).
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

/* NAM (mixer) register offsets, BAR0. */
#define NAM_RESET         0x00
#define NAM_MASTER_VOL    0x02
#define NAM_PCM_VOL       0x18
#define NAM_EXT_AUDIO_ID  0x28
#define NAM_EXT_AUDIO_CTL 0x2A
#define NAM_PCM_DAC_RATE  0x2C
#define EXT_VRA           0x0001

/* NABM (bus master) register offsets, BAR1. PCM Out register box at 0x10. */
#define PO_BDBAR 0x10   /* 32-bit */
#define PO_CIV   0x14   /* 8-bit  */
#define PO_LVI   0x15   /* 8-bit  */
#define PO_SR    0x16   /* 16-bit */
#define PO_PICB  0x18   /* 16-bit */
#define PO_CR    0x1B   /* 8-bit  */
#define GLOB_CNT 0x2C   /* 32-bit */
#define GLOB_STA 0x30   /* 32-bit */

#define SR_DCH   0x01   /* DMA controller halted */
#define CR_RPBM  0x01   /* run/pause bus master  */
#define CR_RR    0x02   /* reset registers       */
#define GLOB_STA_PCR 0x0100   /* primary codec ready */

#define NBUF    32u           /* matches the hardware BDL wrap */
#define BUFSZ   1024u         /* bytes per buffer (512 s16 samples) */

static __attribute__((aligned(8)))  uint8_t  g_bdl[NBUF * 8];
static __attribute__((aligned(16))) uint8_t  g_ring[NBUF * BUFSZ];

static bool     g_present;
static uint16_t g_nam;        /* NAM  I/O base */
static uint16_t g_nabm;       /* NABM I/O base */
static uint8_t  g_fill;       /* next BDL index to fill */
static bool     g_running;
static char     g_name[16];

static inline void  outb(uint16_t p, uint8_t v)  { __asm__ volatile ("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void  outw(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void  outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t v;  __asm__ volatile ("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

static void busy_wait(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) __asm__ volatile ("pause");
}

static void set_bdl(uint8_t idx, const void *buf, uint16_t samples, uint16_t ctl) {
    uint8_t *e = g_bdl + (uint32_t)idx * 8u;
    uint32_t addr = (uint32_t)(uintptr_t)buf;
    e[0] = (uint8_t)addr;       e[1] = (uint8_t)(addr >> 8);
    e[2] = (uint8_t)(addr >> 16);e[3] = (uint8_t)(addr >> 24);
    e[4] = (uint8_t)samples;    e[5] = (uint8_t)(samples >> 8);
    e[6] = (uint8_t)ctl;        e[7] = (uint8_t)(ctl >> 8);
}

static bool probe_pci(struct canboot_pci_addr *out) {
    /* AC'97: class 0x04 (Multimedia) subclass 0x01 (Audio device). */
    uint32_t n = hal_pci_devcount();
    const struct canboot_pci_dev *devs = hal_pci_devs();
    for (uint32_t i = 0; i < n; i++) {
        if (devs[i].class_code == 0x04 && devs[i].subclass == 0x01) {
            *out = devs[i].addr;
            return true;
        }
    }
    return false;
}

bool canboot_ac97_init(void) {
    if (g_present) return true;
    struct canboot_pci_addr addr;
    if (!probe_pci(&addr)) return false;

    hal_pci_enable_bus_master(addr);
    uint64_t bar0 = hal_pci_bar_addr(addr, 0);   /* NAM  */
    uint64_t bar1 = hal_pci_bar_addr(addr, 1);   /* NABM */
    if (!bar0 || !bar1) return false;
    g_nam  = (uint16_t)(bar0 & ~0x3ull);
    g_nabm = (uint16_t)(bar1 & ~0x3ull);

    /* Bring the link out of cold reset and wait for the primary codec. */
    outl(g_nabm + GLOB_CNT, 0x00000002u);
    outw(g_nam + NAM_RESET, 0);                  /* reset the mixer */
    int ready = 0;
    for (int i = 0; i < 1000; i++) {
        if (inl(g_nabm + GLOB_STA) & GLOB_STA_PCR) { ready = 1; break; }
        busy_wait(2000);
    }
    if (!ready) return false;

    /* Unmute + full volume on master and PCM out (0 attenuation). */
    outw(g_nam + NAM_MASTER_VOL, 0x0000);
    outw(g_nam + NAM_PCM_VOL,    0x0000);

    /* Variable-rate audio so we can run at the HAL's 44.1 kHz. */
    if (inw(g_nam + NAM_EXT_AUDIO_ID) & EXT_VRA) {
        outw(g_nam + NAM_EXT_AUDIO_CTL,
             inw(g_nam + NAM_EXT_AUDIO_CTL) | EXT_VRA);
        outw(g_nam + NAM_PCM_DAC_RATE, (uint16_t)HAL_AUDIO_RATE_HZ);
    }

    /* Reset the PCM-out bus master, then point it at our BDL. */
    outb(g_nabm + PO_CR, CR_RR);
    for (int i = 0; i < 1000; i++) {
        if (!(inb(g_nabm + PO_CR) & CR_RR)) break;
        busy_wait(500);
    }
    memset(g_ring, 0, sizeof(g_ring));
    memset(g_bdl, 0, sizeof(g_bdl));
    outl(g_nabm + PO_BDBAR, (uint32_t)(uintptr_t)g_bdl);
    g_fill = 0;
    g_running = false;

    snprintf(g_name, sizeof(g_name), "ac97");
    g_present = true;
    return true;
}

bool canboot_ac97_present(void) { return g_present; }

const char *canboot_ac97_device_name(void) {
    return g_present ? g_name : "none";
}

uint32_t canboot_ac97_write(const int16_t *samples, uint32_t frames) {
    if (!g_present) return frames;
    if (!samples || frames == 0) return 0;
    uint32_t bytes = frames * HAL_AUDIO_CHANNELS * HAL_AUDIO_BPS;
    uint32_t pushed = 0;

    while (pushed < bytes) {
        uint8_t civ = inb(g_nabm + PO_CIV);
        uint8_t queued = (uint8_t)((g_fill - civ) & (NBUF - 1u));
        if (queued >= NBUF - 1u) { busy_wait(500); continue; }  /* ring full */

        uint32_t chunk = bytes - pushed;
        if (chunk > BUFSZ) chunk = BUFSZ;
        uint8_t *dst = g_ring + (uint32_t)g_fill * BUFSZ;
        memcpy(dst, (const uint8_t *)samples + pushed, chunk);

        set_bdl(g_fill, dst, (uint16_t)(chunk / 2u), 0);
        outb(g_nabm + PO_LVI, g_fill);
        g_fill = (uint8_t)((g_fill + 1u) & (NBUF - 1u));
        pushed += chunk;

        /* (Re)start the bus master if it halted or hasn't started. */
        if (!g_running || (inw(g_nabm + PO_SR) & SR_DCH)) {
            outb(g_nabm + PO_CR, CR_RPBM);
            g_running = true;
        }
    }
    return frames;
}

void canboot_ac97_flush(void) {
    if (!g_present) return;
    uint8_t last = (uint8_t)((g_fill - 1u) & (NBUF - 1u));
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint16_t sr = inw(g_nabm + PO_SR);
        if (sr & SR_DCH) return;                 /* drained + halted */
        if (inb(g_nabm + PO_CIV) == last && inw(g_nabm + PO_PICB) < 64) return;
        busy_wait(500);
    }
}

void canboot_ac97_stop(void) {
    if (!g_present) return;
    outb(g_nabm + PO_CR, 0);                     /* clear RPBM */
    g_running = false;
}

#endif /* __x86_64__ */
