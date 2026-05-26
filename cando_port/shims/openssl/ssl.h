#ifndef CANBOOT_SHIM_OPENSSL_SSL_H
#define CANBOOT_SHIM_OPENSSL_SSL_H
/*
 * Bare-metal shim for the slice of the OpenSSL API that cando's
 * lib/sockutil.c (and the socket / secure_socket / http / https libs
 * layered on it) reference. The opaque types are declared here and the
 * sibling headers (bio.h / pem.h / x509.h / x509v3.h / err.h) include
 * this one for them. The functions are backed by cando_port/net_posix
 * (an mbedTLS implementation; a Phase-1 inert stub while the plain TCP
 * path is brought up first).
 */

#include <stddef.h>
#include <stdint.h>

typedef struct ssl_st               SSL;
typedef struct ssl_ctx_st           SSL_CTX;
typedef struct ssl_method_st        SSL_METHOD;
typedef struct bio_st               BIO;
typedef struct x509_st              X509;
typedef struct evp_pkey_st          EVP_PKEY;
typedef struct x509_store_st        X509_STORE;
typedef struct x509_verify_param_st X509_VERIFY_PARAM;

/* Verify modes (bitmask, OpenSSL values). */
#define SSL_VERIFY_NONE                 0x00
#define SSL_VERIFY_PEER                 0x01
#define SSL_VERIFY_FAIL_IF_NO_PEER_CERT 0x02

/* OPENSSL_init_ssl option bits (only need to exist; impl ignores them). */
#define OPENSSL_INIT_LOAD_SSL_STRINGS    0x00200000UL
#define OPENSSL_INIT_LOAD_CRYPTO_STRINGS 0x00000002UL

typedef int (*SSL_verify_cb)(int preverify_ok, void *x509_ctx);

int               OPENSSL_init_ssl(uint64_t opts, const void *settings);

const SSL_METHOD *TLS_client_method(void);
const SSL_METHOD *TLS_server_method(void);

SSL_CTX    *SSL_CTX_new(const SSL_METHOD *method);
void        SSL_CTX_free(SSL_CTX *ctx);
void        SSL_CTX_set_verify(SSL_CTX *ctx, int mode, SSL_verify_cb cb);
int         SSL_CTX_set_default_verify_paths(SSL_CTX *ctx);
int         SSL_CTX_use_certificate(SSL_CTX *ctx, X509 *x);
int         SSL_CTX_add_extra_chain_cert(SSL_CTX *ctx, X509 *x);
int         SSL_CTX_use_PrivateKey(SSL_CTX *ctx, EVP_PKEY *pkey);
int         SSL_CTX_check_private_key(const SSL_CTX *ctx);
X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *ctx);

SSL *SSL_new(SSL_CTX *ctx);
void SSL_free(SSL *ssl);
int  SSL_set_fd(SSL *ssl, int fd);
int  SSL_connect(SSL *ssl);
int  SSL_accept(SSL *ssl);
int  SSL_shutdown(SSL *ssl);
int  SSL_write(SSL *ssl, const void *buf, int num);
int  SSL_read(SSL *ssl, void *buf, int num);

/* In real OpenSSL these are macros over SSL_ctrl / SSL_get0_param; here
 * they are plain functions backed by the shim implementation. */
int                SSL_set_tlsext_host_name(SSL *ssl, const char *name);
X509_VERIFY_PARAM *SSL_get0_param(SSL *ssl);

#endif
