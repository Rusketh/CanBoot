#ifndef CANBOOT_SHIM_OPENSSL_X509_H
#define CANBOOT_SHIM_OPENSSL_X509_H
/* Bare-metal shim: the X509 / private-key / trust-store surface cando's
 * lib/sockutil.c touches while building a TLS context. */

#include <openssl/ssl.h>

void X509_free(X509 *x);
int  X509_STORE_add_cert(X509_STORE *store, X509 *x);
void EVP_PKEY_free(EVP_PKEY *pkey);

#endif
