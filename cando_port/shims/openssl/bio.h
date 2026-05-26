#ifndef CANBOOT_SHIM_OPENSSL_BIO_H
#define CANBOOT_SHIM_OPENSSL_BIO_H
/* Bare-metal shim: the in-memory BIO surface cando's lib/sockutil.c uses
 * to feed PEM blobs into the cert/key parsers. */

#include <openssl/ssl.h>

BIO *BIO_new_mem_buf(const void *buf, int len);
int  BIO_free(BIO *b);

#endif
