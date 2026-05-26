#ifndef CANBOOT_HAL_RNG_H
#define CANBOOT_HAL_RNG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Hardware random number generator. Today this is virtio-rng (a paravirtual
 * entropy source the host feeds from its own RNG); the interface is generic
 * so other backends can register later.
 *
 * It supplements - never replaces - the CPU entropy sources (RDSEED/RDRAND
 * + jitter) the Mbed TLS port mixes for DRBG seeding, so a missing device
 * is harmless.
 */

/* Bring up the RNG device (idempotent). Returns true if one is present. */
bool canboot_rng_init(void);

/* True once a hardware RNG has been found and initialised. */
bool canboot_rng_present(void);

/* Fill `out` with up to `len` random bytes. Returns the number of bytes
 * produced (may be less than len on a transport hiccup), or -1 if no RNG
 * is available. */
int canboot_rng_read(void *out, uint32_t len);

#endif /* CANBOOT_HAL_RNG_H */
