/*
 * lwIP NO_SYS port. The only required platform hook with NO_SYS=1 is
 * sys_now() returning a monotonically increasing millisecond clock.
 *
 * x86_64 uses rdtsc calibrated against the i8254 PIT.
 * aarch64 uses CNTVCT_EL0 / CNTFRQ_EL0 (the architectural generic
 *   timer, no calibration needed - QEMU sets CNTFRQ to 62.5 MHz on
 *   the virt machine, real hardware exposes the true rate).
 *
 * canboot_tsc_hz() returns whichever counter Hz so callers that drive
 * their own deadlines (net_selftest etc.) share the same clock.
 */

#include <stdint.h>

#include "lwip/sys.h"

#if defined(__x86_64__)

static inline uint64_t arch_now(void) {
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

static uint64_t g_clock_hz;

static uint64_t calibrate(void) {
    uint8_t gate = inb(PIT_NMI_PORT);
    outb(PIT_NMI_PORT, (gate & ~0x02u) | 0x01u);
    outb(PIT_CMD, 0xB0);
    const uint16_t reload = 11932;        /* ~10 ms */
    outb(PIT_CHANNEL2, (uint8_t)(reload & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(reload >> 8));
    uint8_t g = inb(PIT_NMI_PORT);
    outb(PIT_NMI_PORT, g & ~0x01u);
    outb(PIT_NMI_PORT, g | 0x01u);
    uint64_t start = arch_now();
    while ((inb(PIT_NMI_PORT) & 0x20u) == 0) {
        __asm__ volatile ("pause");
    }
    uint64_t end = arch_now();
    uint64_t delta = end - start;
    if (delta == 0) return 1000000000ull;
    return delta * 100ull;
}

uint64_t canboot_tsc_hz(void) {
    if (g_clock_hz == 0) g_clock_hz = calibrate();
    return g_clock_hz;
}

#elif defined(__aarch64__)

static inline uint64_t arch_now(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

uint64_t canboot_tsc_hz(void) {
    uint64_t f;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

#else
#error "unsupported architecture for sys_arch"
#endif

u32_t sys_now(void) {
    uint64_t hz = canboot_tsc_hz();
    uint64_t t  = arch_now();
    return (u32_t)((t * 1000ull) / hz);
}

/* LWIP_RAND() source: lwIP uses it to randomise DNS query IDs and the
 * UDP source port. Mix the cycle counter into an LCG so successive calls
 * differ even within the same millisecond. Non-cryptographic, which is
 * all lwIP asks of LWIP_RAND. */
unsigned int canboot_lwip_rand(void) {
    static uint64_t state;
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    state ^= arch_now();
    return (unsigned int)((state >> 33) & 0xFFFFFFFFu);
}
