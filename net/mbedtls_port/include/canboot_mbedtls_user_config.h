/*
 * Canboot user-config for Mbed TLS 3.6.x.
 *
 * Appended after the upstream default mbedtls_config.h via the cmake
 * MBEDTLS_USER_CONFIG_FILE option. We keep the upstream default's crypto
 * + TLS feature set and only disable features that require POSIX
 * services we don't ship (sockets, filesystem, gettimeofday, pthread,
 * select-based timing) plus the platform entropy source (we provide our
 * own via the hardware-poll alt hook).
 */

#ifndef CANBOOT_MBEDTLS_USER_CONFIG_H
#define CANBOOT_MBEDTLS_USER_CONFIG_H

/* Hardware AES accelerators. On x86_64 AESNI compiles fine under the
 * default freestanding flags; on aarch64 the AESCE path needs
 * -march=armv8-a+crypto+sha2 which we don't pass (it'd cascade through
 * the rest of the libc/kernel build). Disable AESCE on aarch64 so
 * mbedtls falls back to the portable C AES implementation. */
#if defined(__aarch64__)
#undef MBEDTLS_AESCE_C
#endif

/* POSIX dependencies */
#undef MBEDTLS_NET_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_THREADING_C
#undef MBEDTLS_THREADING_PTHREAD
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C  /* persistent key storage needs FS */

/* Use our own entropy source: hardware-poll backed by RDRAND/RDSEED
 * + TSC jitter; no /dev/random or getrandom() syscall available.
 * MBEDTLS_NO_PLATFORM_ENTROPY blocks the platform poll; the hardware
 * alt is auto-registered by entropy_init() when ENTROPY_HARDWARE_ALT
 * is set and NO_DEFAULT_ENTROPY_SOURCES is *not* set. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#endif /* CANBOOT_MBEDTLS_USER_CONFIG_H */
