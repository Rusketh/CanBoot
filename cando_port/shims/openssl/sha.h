#ifndef CANBOOT_SHIM_OPENSSL_SHA_H
#define CANBOOT_SHIM_OPENSSL_SHA_H
/* Bare-metal shim: just the SHA-256 digest length cando's
 * lib/secure_socket.c uses to size its fingerprint buffer. */
#define SHA256_DIGEST_LENGTH 32
#endif
