/*
 * OpenSSL-API-over-Mbed TLS shim.
 *
 * Backs the OpenSSL surface cando's lib/sockutil.c + lib/secure_socket.c
 * link against (declared in cando_port/shims/openssl/*) with Mbed TLS
 * 3.6.x. TLS records ride the BSD socket fd from cando_port/net_posix/
 * sockets.c via send()/recv() BIO callbacks, so secure_socket layers
 * cleanly on the same lwIP-backed transport the plain `socket` library
 * uses.
 *
 * Object model:
 *   SSL_CTX  -> endpoint + authmode + (CA chain, own cert, own key)
 *   SSL      -> per-connection mbedtls_ssl_context/config + the fd
 *   X509     -> wrapper over mbedtls_x509_crt (owned when parsed from PEM,
 *               borrowed when returned by SSL_get_peer_certificate)
 *   EVP_PKEY -> wrapper over mbedtls_pk_context
 *   BIO      -> in-memory: a read source (PEM) or a write sink
 *               (ASN1_TIME_print)
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/socket.h>   /* send / recv on the socket fd */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha256.h"

/* ---- one-time RNG ----------------------------------------------------- */

static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int                      g_rng_ready;

int OPENSSL_init_ssl(uint64_t opts, const void *settings) {
    (void)opts; (void)settings;
    if (!g_rng_ready) {
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_drbg);
        if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                (const unsigned char *)"canboot-sockutil", 16) == 0)
            g_rng_ready = 1;
    }
    return 1;
}

/* ---- method sentinels ------------------------------------------------- */

#define M_CLIENT ((const SSL_METHOD *)1)
#define M_SERVER ((const SSL_METHOD *)2)

const SSL_METHOD *TLS_client_method(void) { return M_CLIENT; }
const SSL_METHOD *TLS_server_method(void) { return M_SERVER; }

/* ---- structures ------------------------------------------------------- */

struct ssl_ctx_st {
    int                 endpoint;     /* MBEDTLS_SSL_IS_CLIENT / _SERVER */
    int                 authmode;     /* MBEDTLS_SSL_VERIFY_*            */
    mbedtls_x509_crt   *ca;           /* trust anchors, owned           */
    mbedtls_x509_crt   *own_cert;     /* leaf (+chain), owned           */
    mbedtls_pk_context *own_key;      /* owned                          */
};

struct ssl_st {
    SSL_CTX                *ctx;
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    int                     conf_inited;
    int                     setup_done;
    int                     fd;
};

struct x509_st {
    mbedtls_x509_crt *crt;
    int               owned;          /* free crt on X509_free when set */
};

struct evp_pkey_st {
    mbedtls_pk_context *pk;
    int                 owned;
};

struct bio_st {
    unsigned char *data;   /* read source: owned copy, NUL-terminated   */
    size_t         len;    /* includes the NUL (mbedtls PEM convention) */
    int            consumed;
    char          *wbuf;   /* write sink                                */
    size_t         wlen, wcap;
    int            sink;
};

/* ---- SSL_CTX ---------------------------------------------------------- */

SSL_CTX *SSL_CTX_new(const SSL_METHOD *m) {
    SSL_CTX *c = (SSL_CTX *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->endpoint = (m == M_SERVER) ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT;
    c->authmode = MBEDTLS_SSL_VERIFY_NONE;
    return c;
}

void SSL_CTX_free(SSL_CTX *c) {
    if (!c) return;
    if (c->ca)       { mbedtls_x509_crt_free(c->ca);       free(c->ca); }
    if (c->own_cert) { mbedtls_x509_crt_free(c->own_cert); free(c->own_cert); }
    if (c->own_key)  { mbedtls_pk_free(c->own_key);        free(c->own_key); }
    free(c);
}

void SSL_CTX_set_verify(SSL_CTX *c, int mode, SSL_verify_cb cb) {
    (void)cb;
    if (!c) return;
    c->authmode = (mode & SSL_VERIFY_PEER) ? MBEDTLS_SSL_VERIFY_REQUIRED
                                           : MBEDTLS_SSL_VERIFY_NONE;
}

/* No system trust store on bare metal; explicit CAs are added via
 * SSL_CTX_get_cert_store + X509_STORE_add_cert. */
int SSL_CTX_set_default_verify_paths(SSL_CTX *c) { (void)c; return 1; }

int SSL_CTX_use_certificate(SSL_CTX *c, X509 *x) {
    if (!c || !x || !x->crt) return 0;
    if (c->own_cert) { mbedtls_x509_crt_free(c->own_cert); free(c->own_cert); }
    c->own_cert = x->crt;
    x->owned    = 0;     /* ownership moves into the ctx */
    return 1;
}

int SSL_CTX_add_extra_chain_cert(SSL_CTX *c, X509 *x) {
    if (!c || !x || !x->crt) return 0;
    if (!c->own_cert) {
        c->own_cert = x->crt;
    } else {
        mbedtls_x509_crt *p = c->own_cert;
        while (p->next) p = p->next;
        p->next = x->crt;
    }
    x->owned = 0;
    return 1;
}

int SSL_CTX_use_PrivateKey(SSL_CTX *c, EVP_PKEY *k) {
    if (!c || !k || !k->pk) return 0;
    if (c->own_key) { mbedtls_pk_free(c->own_key); free(c->own_key); }
    c->own_key = k->pk;
    k->owned   = 0;
    return 1;
}

int SSL_CTX_check_private_key(const SSL_CTX *c) {
    if (!c || !c->own_cert || !c->own_key) return 0;
    return mbedtls_pk_check_pair(&c->own_cert->pk, c->own_key,
                                 mbedtls_ctr_drbg_random, &g_drbg) == 0;
}

X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *c) {
    return (X509_STORE *)(uintptr_t)c;
}

int X509_STORE_add_cert(X509_STORE *store, X509 *x) {
    SSL_CTX *c = (SSL_CTX *)store;
    if (!c || !x || !x->crt) return 0;
    if (!c->ca) {
        c->ca = x->crt;
    } else {
        mbedtls_x509_crt *p = c->ca;
        while (p->next) p = p->next;
        p->next = x->crt;
    }
    x->owned = 0;
    return 1;
}

/* ---- SSL -------------------------------------------------------------- */

SSL *SSL_new(SSL_CTX *c) {
    if (!c) return NULL;
    SSL *s = (SSL *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx = c;
    s->fd  = -1;
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    if (mbedtls_ssl_config_defaults(&s->conf, c->endpoint,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto fail;
    s->conf_inited = 1;
    mbedtls_ssl_conf_authmode(&s->conf, c->authmode);
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &g_drbg);
    if (c->ca) mbedtls_ssl_conf_ca_chain(&s->conf, c->ca, NULL);
    if (c->own_cert && c->own_key &&
        mbedtls_ssl_conf_own_cert(&s->conf, c->own_cert, c->own_key) != 0)
        goto fail;
    if (mbedtls_ssl_setup(&s->ssl, &s->conf) != 0) goto fail;
    s->setup_done = 1;
    return s;
fail:
    SSL_free(s);
    return NULL;
}

void SSL_free(SSL *s) {
    if (!s) return;
    mbedtls_ssl_free(&s->ssl);
    if (s->conf_inited) mbedtls_ssl_config_free(&s->conf);
    free(s);
}

int SSL_set_fd(SSL *s, int fd) {
    if (!s) return 0;
    s->fd = fd;
    return 1;
}

int SSL_set_tlsext_host_name(SSL *s, const char *name) {
    if (!s || !name) return 0;
    return mbedtls_ssl_set_hostname(&s->ssl, name) == 0;
}

X509_VERIFY_PARAM *SSL_get0_param(SSL *s) { return (X509_VERIFY_PARAM *)s; }

void X509_VERIFY_PARAM_set_hostflags(X509_VERIFY_PARAM *p, unsigned int f) {
    (void)p; (void)f;
}

int X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM *p, const char *name, size_t len) {
    SSL *s = (SSL *)p;
    if (!s || !name) return 0;
    char tmp[256];
    if (len == 0) len = strlen(name);
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, name, len);
    tmp[len] = '\0';
    return mbedtls_ssl_set_hostname(&s->ssl, tmp) == 0;
}

/* BIO callbacks over the BSD socket fd. The fd's recv() blocks (pumping
 * lwIP) until data arrives, so the handshake completes synchronously. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int  fd = *(int *)ctx;
    long n  = send(fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN) ? MBEDTLS_ERR_SSL_WANT_WRITE : -1;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int  fd = *(int *)ctx;
    long n  = recv(fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN) ? MBEDTLS_ERR_SSL_WANT_READ : -1;
    if (n == 0) return 0;   /* clean EOF */
    return (int)n;
}

int SSL_connect(SSL *s) {
    if (!s || !s->setup_done) return -1;
    mbedtls_ssl_set_bio(&s->ssl, &s->fd, bio_send, bio_recv, NULL);
    int rc;
    while ((rc = mbedtls_ssl_handshake(&s->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            return -1;
    }
    return 1;
}

int SSL_accept(SSL *s) { return SSL_connect(s); }

int SSL_read(SSL *s, void *buf, int num) {
    if (!s || num <= 0) return -1;
    int n;
    do { n = mbedtls_ssl_read(&s->ssl, (unsigned char *)buf, (size_t)num); }
    while (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    return n < 0 ? -1 : n;
}

int SSL_write(SSL *s, const void *buf, int num) {
    if (!s || num <= 0) return -1;
    int n;
    do { n = mbedtls_ssl_write(&s->ssl, (const unsigned char *)buf, (size_t)num); }
    while (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE);
    return n < 0 ? -1 : n;
}

int SSL_shutdown(SSL *s) {
    if (s) mbedtls_ssl_close_notify(&s->ssl);
    return 1;
}

const char *SSL_get_version(const SSL *s) {
    return s ? mbedtls_ssl_get_version(&((SSL *)s)->ssl) : NULL;
}

const char *SSL_get_cipher_name(const SSL *s) {
    return s ? mbedtls_ssl_get_ciphersuite(&((SSL *)s)->ssl) : NULL;
}

X509 *SSL_get_peer_certificate(const SSL *s) {
    if (!s) return NULL;
    const mbedtls_x509_crt *pc = mbedtls_ssl_get_peer_cert(&((SSL *)s)->ssl);
    if (!pc) return NULL;
    X509 *x = (X509 *)calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->crt   = (mbedtls_x509_crt *)pc;   /* borrowed: owned by the ssl ctx */
    x->owned = 0;
    return x;
}

/* ---- X509 ------------------------------------------------------------- */

void X509_free(X509 *x) {
    if (!x) return;
    if (x->owned && x->crt) { mbedtls_x509_crt_free(x->crt); free(x->crt); }
    free(x);
}

X509_NAME *X509_get_subject_name(X509 *x) {
    return (x && x->crt) ? (X509_NAME *)&x->crt->subject : NULL;
}

X509_NAME *X509_get_issuer_name(X509 *x) {
    return (x && x->crt) ? (X509_NAME *)&x->crt->issuer : NULL;
}

char *X509_NAME_oneline(X509_NAME *name, char *buf, int size) {
    if (!buf || size <= 0) return buf;
    if (!name) { buf[0] = '\0'; return buf; }
    int n = mbedtls_x509_dn_gets(buf, (size_t)size, (const mbedtls_x509_name *)name);
    if (n < 0) buf[0] = '\0';
    return buf;
}

const ASN1_TIME *X509_get0_notBefore(const X509 *x) {
    return (x && x->crt) ? (const ASN1_TIME *)&x->crt->valid_from : NULL;
}

const ASN1_TIME *X509_get0_notAfter(const X509 *x) {
    return (x && x->crt) ? (const ASN1_TIME *)&x->crt->valid_to : NULL;
}

static void bio_mem_write(BIO *b, const char *data, int n);

int ASN1_TIME_print(BIO *bp, const ASN1_TIME *t) {
    if (!bp || !t) return 0;
    const mbedtls_x509_time *tm = (const mbedtls_x509_time *)t;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                     tm->year, tm->mon, tm->day, tm->hour, tm->min, tm->sec);
    if (n > 0) bio_mem_write(bp, buf, n);
    return 1;
}

const EVP_MD *EVP_sha256(void) { return (const EVP_MD *)1; }

int X509_digest(const X509 *x, const EVP_MD *md,
                unsigned char *out, unsigned int *outlen) {
    (void)md;
    if (!x || !x->crt || !out) return 0;
    if (mbedtls_sha256(x->crt->raw.p, x->crt->raw.len, out, 0) != 0) return 0;
    if (outlen) *outlen = 32;
    return 1;
}

void EVP_PKEY_free(EVP_PKEY *k) {
    if (!k) return;
    if (k->owned && k->pk) { mbedtls_pk_free(k->pk); free(k->pk); }
    free(k);
}

/* ---- BIO -------------------------------------------------------------- */

BIO *BIO_new_mem_buf(const void *buf, int len) {
    if (len < 0) len = buf ? (int)strlen((const char *)buf) : 0;
    BIO *b = (BIO *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->data = (unsigned char *)malloc((size_t)len + 1);
    if (!b->data) { free(b); return NULL; }
    if (buf && len) memcpy(b->data, buf, (size_t)len);
    b->data[len] = '\0';
    b->len = (size_t)len + 1;   /* include NUL: mbedtls PEM parse needs it */
    return b;
}

const BIO_METHOD *BIO_s_mem(void) { return (const BIO_METHOD *)1; }

BIO *BIO_new(const BIO_METHOD *m) {
    (void)m;
    BIO *b = (BIO *)calloc(1, sizeof(*b));
    if (b) b->sink = 1;
    return b;
}

static void bio_mem_write(BIO *b, const char *data, int n) {
    if (!b || n <= 0) return;
    size_t need = b->wlen + (size_t)n + 1;
    if (need > b->wcap) {
        size_t ncap = b->wcap ? b->wcap * 2 : 64;
        while (ncap < need) ncap *= 2;
        char *nb = (char *)realloc(b->wbuf, ncap);
        if (!nb) return;
        b->wbuf = nb;
        b->wcap = ncap;
    }
    memcpy(b->wbuf + b->wlen, data, (size_t)n);
    b->wlen += (size_t)n;
    b->wbuf[b->wlen] = '\0';
}

long BIO_get_mem_data(BIO *b, char **pp) {
    if (!b) { if (pp) *pp = NULL; return 0; }
    if (pp) *pp = b->wbuf;
    return (long)b->wlen;
}

int BIO_free(BIO *b) {
    if (!b) return 0;
    free(b->data);
    free(b->wbuf);
    free(b);
    return 1;
}

/* ---- PEM readers ------------------------------------------------------ */

X509 *PEM_read_bio_X509(BIO *bp, X509 **out, pem_password_cb *cb, void *u) {
    (void)out; (void)cb; (void)u;
    if (!bp || bp->consumed) return NULL;
    bp->consumed = 1;       /* read-once: mbedtls parses the whole chain */
    mbedtls_x509_crt *crt = (mbedtls_x509_crt *)calloc(1, sizeof(*crt));
    if (!crt) return NULL;
    mbedtls_x509_crt_init(crt);
    if (mbedtls_x509_crt_parse(crt, bp->data, bp->len) != 0) {
        mbedtls_x509_crt_free(crt);
        free(crt);
        return NULL;
    }
    X509 *x = (X509 *)calloc(1, sizeof(*x));
    if (!x) { mbedtls_x509_crt_free(crt); free(crt); return NULL; }
    x->crt   = crt;
    x->owned = 1;
    return x;
}

EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **out,
                                  pem_password_cb *cb, void *u) {
    (void)out; (void)cb; (void)u;
    if (!bp || bp->consumed) return NULL;
    bp->consumed = 1;
    mbedtls_pk_context *pk = (mbedtls_pk_context *)calloc(1, sizeof(*pk));
    if (!pk) return NULL;
    mbedtls_pk_init(pk);
    if (mbedtls_pk_parse_key(pk, bp->data, bp->len, NULL, 0,
                             mbedtls_ctr_drbg_random, &g_drbg) != 0) {
        mbedtls_pk_free(pk);
        free(pk);
        return NULL;
    }
    EVP_PKEY *k = (EVP_PKEY *)calloc(1, sizeof(*k));
    if (!k) { mbedtls_pk_free(pk); free(pk); return NULL; }
    k->pk    = pk;
    k->owned = 1;
    return k;
}

/* ---- error strings ---------------------------------------------------- */

unsigned long ERR_peek_last_error(void) { return 0; }

void ERR_error_string_n(unsigned long e, char *buf, size_t len) {
    (void)e;
    if (buf && len) buf[0] = '\0';
}
