#ifndef CANBOOT_SHIM_OPENSSL_PEM_H
#define CANBOOT_SHIM_OPENSSL_PEM_H
/* Bare-metal shim: the PEM readers cando's lib/sockutil.c uses to pull a
 * cert (or chain) and a private key out of a memory BIO. */

#include <openssl/ssl.h>

typedef int (*pem_password_cb)(char *buf, int size, int rwflag, void *u);

X509     *PEM_read_bio_X509(BIO *bp, X509 **x, pem_password_cb *cb, void *u);
EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **x,
                                  pem_password_cb *cb, void *u);

#endif
