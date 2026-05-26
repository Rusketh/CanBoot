/*
 * Phase-1 inert OpenSSL surface.
 *
 * cando's lib/sockutil.c links against the OpenSSL TLS API even when only
 * the plain-TCP path is exercised (socket_pool_release / sockutil_tls_*
 * reference the symbols unconditionally). This file satisfies those
 * symbols with failing no-ops so the `socket` library can be brought up
 * and verified end-to-end over plain TCP first. The TLS path is wired to
 * a real Mbed TLS backing in a follow-up step (replacing this file); the
 * shim headers in cando_port/shims/openssl/ stay as the stable surface.
 *
 * Nothing here is reachable from the plain-TCP flow: secure_socket /
 * https are not registered yet, and SocketSlot::ssl / ::ssl_ctx are NULL
 * for every plain socket, so sockutil_tls_free / SSL_CTX_free are never
 * called with a live handle.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

int OPENSSL_init_ssl(uint64_t opts, const void *settings) {
    (void)opts; (void)settings;
    return 1;   /* OpenSSL 1.1+ auto-init: report success */
}

const SSL_METHOD *TLS_client_method(void) { return NULL; }
const SSL_METHOD *TLS_server_method(void) { return NULL; }

SSL_CTX    *SSL_CTX_new(const SSL_METHOD *m) { (void)m; return NULL; }
void        SSL_CTX_free(SSL_CTX *ctx) { (void)ctx; }
void        SSL_CTX_set_verify(SSL_CTX *ctx, int mode, SSL_verify_cb cb) {
    (void)ctx; (void)mode; (void)cb;
}
int         SSL_CTX_set_default_verify_paths(SSL_CTX *ctx) { (void)ctx; return 0; }
int         SSL_CTX_use_certificate(SSL_CTX *ctx, X509 *x) { (void)ctx; (void)x; return 0; }
int         SSL_CTX_add_extra_chain_cert(SSL_CTX *ctx, X509 *x) { (void)ctx; (void)x; return 0; }
int         SSL_CTX_use_PrivateKey(SSL_CTX *ctx, EVP_PKEY *k) { (void)ctx; (void)k; return 0; }
int         SSL_CTX_check_private_key(const SSL_CTX *ctx) { (void)ctx; return 0; }
X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *ctx) { (void)ctx; return NULL; }

SSL *SSL_new(SSL_CTX *ctx) { (void)ctx; return NULL; }
void SSL_free(SSL *ssl) { (void)ssl; }
int  SSL_set_fd(SSL *ssl, int fd) { (void)ssl; (void)fd; return 0; }
int  SSL_connect(SSL *ssl) { (void)ssl; return -1; }
int  SSL_accept(SSL *ssl) { (void)ssl; return -1; }
int  SSL_shutdown(SSL *ssl) { (void)ssl; return 0; }
int  SSL_write(SSL *ssl, const void *buf, int num) { (void)ssl; (void)buf; (void)num; return -1; }
int  SSL_read(SSL *ssl, void *buf, int num) { (void)ssl; (void)buf; (void)num; return -1; }

int                SSL_set_tlsext_host_name(SSL *ssl, const char *name) {
    (void)ssl; (void)name; return 0;
}
X509_VERIFY_PARAM *SSL_get0_param(SSL *ssl) { (void)ssl; return NULL; }

void X509_VERIFY_PARAM_set_hostflags(X509_VERIFY_PARAM *p, unsigned int f) {
    (void)p; (void)f;
}
int  X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM *p, const char *n, size_t l) {
    (void)p; (void)n; (void)l; return 0;
}

BIO *BIO_new_mem_buf(const void *buf, int len) { (void)buf; (void)len; return NULL; }
int  BIO_free(BIO *b) { (void)b; return 0; }

X509     *PEM_read_bio_X509(BIO *bp, X509 **x, pem_password_cb *cb, void *u) {
    (void)bp; (void)x; (void)cb; (void)u; return NULL;
}
EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **x,
                                  pem_password_cb *cb, void *u) {
    (void)bp; (void)x; (void)cb; (void)u; return NULL;
}

void X509_free(X509 *x) { (void)x; }
int  X509_STORE_add_cert(X509_STORE *s, X509 *x) { (void)s; (void)x; return 0; }
void EVP_PKEY_free(EVP_PKEY *k) { (void)k; }

unsigned long ERR_peek_last_error(void) { return 0; }
void ERR_error_string_n(unsigned long e, char *buf, size_t len) {
    (void)e;
    if (buf && len) buf[0] = '\0';
}
