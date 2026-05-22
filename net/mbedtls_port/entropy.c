/*
 * Hardware entropy source for Mbed TLS.
 *
 * Tries RDSEED, then RDRAND, then falls back to a TSC-jitter bit-mixer.
 * Mbed TLS's MBEDTLS_ENTROPY_HARDWARE_ALT path calls this until it has
 * enough bytes to seed CTR_DRBG; it's not the long-term RNG (CTR_DRBG
 * is) so even a modest mixer is acceptable for boot-time use.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mbedtls/entropy.h"

/* CPUID-based feature detection so we don't #UD on QEMU's qemu64 CPU
 * (no RDRAND, no RDSEED). Detected once on first call. */
static int g_features_probed;
static int g_has_rdrand;
static int g_has_rdseed;

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "0"(leaf), "2"(subleaf));
}

static void probe_features(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;

    if (max_leaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        g_has_rdrand = (c & (1u << 30)) != 0;   /* CPUID.1:ECX[30] */
    }
    if (max_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_has_rdseed = (b & (1u << 18)) != 0;   /* CPUID.7.0:EBX[18] */
    }
    g_features_probed = 1;
}

static inline int rdseed64(uint64_t *out) {
    if (!g_has_rdseed) return 0;
    unsigned char ok;
    uint64_t v;
    __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
    if (!ok) return 0;
    *out = v;
    return 1;
}

static inline int rdrand64(uint64_t *out) {
    if (!g_has_rdrand) return 0;
    unsigned char ok;
    uint64_t v;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    if (!ok) return 0;
    *out = v;
    return 1;
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t jitter_word(void) {
    /* Mix many short rdtsc deltas into a single uint64_t. The HLT-less
     * "pause" loop varies subtly with cache/TLB/branch state, giving
     * a few bits of entropy per iteration on real hardware. */
    uint64_t acc = rdtsc();
    for (int i = 0; i < 64; i++) {
        uint64_t t0 = rdtsc();
        for (volatile int j = 0; j < 100; j++) { __asm__ volatile ("pause"); }
        uint64_t t1 = rdtsc();
        acc = (acc << 1) ^ (acc >> 63) ^ (t1 - t0);
    }
    return acc;
}

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen) {
    (void)data;
    if (!g_features_probed) probe_features();
    size_t produced = 0;
    while (produced < len) {
        uint64_t v;
        if (!rdseed64(&v) && !rdrand64(&v)) {
            v = jitter_word();
        }
        size_t chunk = (len - produced) < sizeof(v) ? (len - produced) : sizeof(v);
        memcpy(output + produced, &v, chunk);
        produced += chunk;
    }
    *olen = produced;
    return 0;
}
