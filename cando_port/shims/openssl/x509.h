#ifndef CANBOOT_SHIM_OPENSSL_X509_H
#define CANBOOT_SHIM_OPENSSL_X509_H
/* Bare-metal shim: the X509 / private-key / trust-store surface cando's
 * lib/sockutil.c and lib/secure_socket.c touch while building a TLS
 * context and inspecting a peer certificate. */

#include <openssl/ssl.h>

typedef struct x509_name_st X509_NAME;
typedef struct asn1_time_st ASN1_TIME;
typedef struct evp_md_st    EVP_MD;

void X509_free(X509 *x);
int  X509_STORE_add_cert(X509_STORE *store, X509 *x);
void EVP_PKEY_free(EVP_PKEY *pkey);

/* Peer-certificate inspection (secure_socket.c peerCertificate()). */
X509_NAME      *X509_get_subject_name(X509 *x);
X509_NAME      *X509_get_issuer_name(X509 *x);
char           *X509_NAME_oneline(X509_NAME *name, char *buf, int size);
const ASN1_TIME *X509_get0_notBefore(const X509 *x);
const ASN1_TIME *X509_get0_notAfter(const X509 *x);
int             ASN1_TIME_print(BIO *bp, const ASN1_TIME *t);
const EVP_MD   *EVP_sha256(void);
int             X509_digest(const X509 *x, const EVP_MD *md,
                            unsigned char *out, unsigned int *outlen);

#endif
