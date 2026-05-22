#ifndef CANBOOT_SHIM_OPENSSL_SSL_H
#define CANBOOT_SHIM_OPENSSL_SSL_H
/* Bare-metal shim: declares the opaque types cando's lib/sockutil.h and
 * lib/httputil.h reference in struct fields and function signatures.
 * No real OpenSSL functions are exposed; the .c files that use them are
 * not compiled into our build. */

typedef struct ssl_st     SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;
typedef struct bio_st     BIO;
typedef struct x509_st    X509;
typedef struct evp_pkey_st EVP_PKEY;

#endif
