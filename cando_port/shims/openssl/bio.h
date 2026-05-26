#ifndef CANBOOT_SHIM_OPENSSL_BIO_H
#define CANBOOT_SHIM_OPENSSL_BIO_H
/* Bare-metal shim: the in-memory BIO surface cando's lib/sockutil.c and
 * lib/secure_socket.c use to feed PEM blobs into the cert/key parsers and
 * to capture ASN1_TIME_print output. */

#include <openssl/ssl.h>

typedef struct bio_method_st BIO_METHOD;

BIO *BIO_new_mem_buf(const void *buf, int len);
int  BIO_free(BIO *b);

/* Writable in-memory BIO (sink for ASN1_TIME_print). */
const BIO_METHOD *BIO_s_mem(void);
BIO              *BIO_new(const BIO_METHOD *method);
long              BIO_get_mem_data(BIO *b, char **pp);

#endif
