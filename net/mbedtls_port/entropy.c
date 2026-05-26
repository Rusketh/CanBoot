/*
 * Hardware entropy source for Mbed TLS.
 *
 * x86_64: tries RDSEED, then RDRAND, then falls back to a TSC-jitter
 *   bit-mixer. RDRAND/RDSEED are CPUID-gated so we don't #UD on
 *   QEMU's qemu64 CPU.
 *
 * aarch64: cortex-a72 (the QEMU virt default) predates FEAT_RNG
 *   (ARMv8.5+), so RNDR/RNDRSS aren't available. We always use a
 *   CNTVCT_EL0 jitter mixer; a future patch can probe ID_AA64ISAR0_EL1.
 *
 * Mbed TLS's MBEDTLS_ENTROPY_HARDWARE_ALT path calls this until it has
 * enough bytes to seed CTR_DRBG; it's not the long-term RNG (CTR_DRBG
 * is) so even a modest mixer is acceptable for boot-time use.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mbedtls/entropy.h"
#include "hal/rng.h"

#if defined(__x86_64__)

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
        g_has_rdrand = (c & (1u << 30)) != 0;
    }
    if (max_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_has_rdseed = (b & (1u << 18)) != 0;
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

static inline uint64_t arch_clock(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void arch_relax(void) {
    __asm__ volatile ("pause");
}

#elif defined(__aarch64__)

static int g_features_probed;
/* RNDR / RNDRSS detection deferred; QEMU virt + cortex-a72 lacks
 * FEAT_RNG so we always fall through to the jitter mixer. */

static inline int rdseed64(uint64_t *out) { (void)out; return 0; }
static inline int rdrand64(uint64_t *out) { (void)out; return 0; }

static void probe_features(void) { g_features_probed = 1; }

static inline uint64_t arch_clock(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline void arch_relax(void) {
    __asm__ volatile ("yield");
}

#else
#error "unsupported architecture for entropy"
#endif

static uint64_t jitter_word(void) {
    /* Mix many short clock deltas into a single uint64_t. The relax
     * loop varies subtly with cache/TLB/branch state, giving a few
     * bits of entropy per iteration. */
    uint64_t acc = arch_clock();
    for (int i = 0; i < 64; i++) {
        uint64_t t0 = arch_clock();
        for (volatile int j = 0; j < 100; j++) { arch_relax(); }
        uint64_t t1 = arch_clock();
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

    /* If a hardware RNG (virtio-rng) is present, fold its bytes into the
     * output. XOR can only add entropy, so this never weakens the CPU/jitter
     * source above, and a missing device is a no-op. */
    if (canboot_rng_present() || canboot_rng_init()) {
        unsigned char hw[64];
        size_t off = 0;
        while (off < produced) {
            size_t want = produced - off;
            if (want > sizeof(hw)) want = sizeof(hw);
            int got = canboot_rng_read(hw, (uint32_t)want);
            if (got <= 0) break;
            for (int i = 0; i < got; i++) output[off + (size_t)i] ^= hw[i];
            off += (size_t)got;
        }
    }

    *olen = produced;
    return 0;
}
