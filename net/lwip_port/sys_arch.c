/*
 * lwIP NO_SYS port. With NO_SYS=1 the only required platform hook is
 * sys_now() returning a monotonically increasing millisecond clock.
 * Backed by rdtsc; the CPU frequency is calibrated against the i8254
 * PIT once on first call so timeouts stay reasonably accurate even
 * when the host CPU is faster/slower than 1 GHz.
 */

#include <stdint.h>

#include "lwip/sys.h"

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define PIT_CHANNEL2 0x42
#define PIT_CMD      0x43
#define PIT_NMI_PORT 0x61
#define PIT_HZ       1193182u

static uint64_t g_tsc_hz;

static uint64_t calibrate_tsc(void) {
    /* Configure i8254 channel 2 to count down a fixed interval, gate it on
     * via port 0x61, sample tsc deltas, derive frequency. */
    uint8_t gate = inb(PIT_NMI_PORT);
    outb(PIT_NMI_PORT, (gate & ~0x02u) | 0x01u);  /* gate-low, speaker off */

    outb(PIT_CMD, 0xB0);  /* ch2, lobyte/hibyte, mode 0, binary */

    /* ~10 ms = 11932 ticks at 1.193182 MHz */
    const uint16_t reload = 11932;
    outb(PIT_CHANNEL2, (uint8_t)(reload & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(reload >> 8));

    /* Pulse gate to start counting. */
    uint8_t g = inb(PIT_NMI_PORT);
    outb(PIT_NMI_PORT, g & ~0x01u);
    outb(PIT_NMI_PORT, g | 0x01u);

    uint64_t start = rdtsc();
    while ((inb(PIT_NMI_PORT) & 0x20u) == 0) {
        __asm__ volatile ("pause");
    }
    uint64_t end = rdtsc();

    uint64_t delta = end - start;
    if (delta == 0) return 1000000000ull;  /* fallback 1 GHz */
    return delta * 100ull;  /* 10 ms sample -> Hz = delta * 100 */
}

u32_t sys_now(void) {
    if (g_tsc_hz == 0) {
        g_tsc_hz = calibrate_tsc();
    }
    /* Return rdtsc / (tsc_hz / 1000) = rdtsc * 1000 / tsc_hz */
    uint64_t t = rdtsc();
    return (u32_t)((t * 1000ull) / g_tsc_hz);
}

uint64_t canboot_tsc_hz(void) {
    if (g_tsc_hz == 0) g_tsc_hz = calibrate_tsc();
    return g_tsc_hz;
}
