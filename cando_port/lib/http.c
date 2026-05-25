/*
 * cando http / https modules - URL-aware HTTP(S) clients on top of
 * net.httpGet / tls.httpsGet. The plain net/tls libs take split
 * host/port/path; these accept a single URL string.
 *
 *   http.get("http://10.0.2.2:8080/")   -> body or null
 *   http.post("http://...", body)        -> body or null  (TBD)
 *   https.get("https://10.0.2.2:8443/")  -> body or null
 *
 * Replaces cando's built-in http/https libs (which need libcurl /
 * OpenSSL we don't have). The vendored register hooks are stubbed in
 * cando_stubs.c so our open_*lib funcs win the global slot.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "lwip/ip4_addr.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

/* Forward declarations for the low-level helpers in the existing
 * net + tls libs. Each parses a dotted-quad host, opens a socket via
 * lwIP, and returns the response body as a C string in a static
 * buffer (which we copy here). */
struct net_get_result {
    const char *body;
    size_t      len;
    int         status;
};

/* Minimal URL parser - same shape as cando_url_lib's. Inline so the
 * file is self-contained for the HTTP/HTTPS path. */
struct url_parts {
    const char *scheme;   size_t scheme_n;
    char        host[64];
    int         port;
    const char *path;
};

static int parse_url(const char *url, struct url_parts *p) {
    if (!url) return -1;
    const char *colon = strstr(url, "://");
    if (!colon) return -1;
    p->scheme = url;
    p->scheme_n = (size_t)(colon - url);
    const char *s = colon + 3;
    /* host */
    size_t host_n = 0;
    while (*s && *s != ':' && *s != '/' && host_n < sizeof(p->host) - 1) {
        p->host[host_n++] = *s++;
    }
    p->host[host_n] = '\0';
    /* port */
    p->port = 0;
    if (*s == ':') {
        s++;
        while (*s >= '0' && *s <= '9') { p->port = p->port * 10 + (*s - '0'); s++; }
    }
    if (p->port == 0) {
        if (p->scheme_n == 5 && memcmp(p->scheme, "https", 5) == 0) p->port = 443;
        else                                                        p->port = 80;
    }
    /* path */
    p->path = (*s == '/') ? s : "/";
    return 0;
}

/* Pull in the same UDP / TCP / TLS helpers as their respective libs
 * via a trampoline call. We reuse the existing one-shot functions by
 * declaring them here and calling through them; they're not exposed
 * publicly but live in cando_net_lib.c / cando_tls_lib.c. To keep
 * things simple we instead reimplement the GET path locally via the
 * same lwIP/mbedtls calls those libs use. */

#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "hal/net.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "lwip_bio.h"

extern const char    canboot_test_ca_pem[];
extern const size_t  canboot_test_ca_pem_len;
extern uint64_t      canboot_tsc_hz(void);

static inline uint64_t arch_now(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#endif
}

static inline void arch_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#endif
}

static bool wait_flag(volatile bool *f, uint32_t timeout_ms) {
    uint64_t dl = arch_now() + (canboot_tsc_hz() * timeout_ms) / 1000ull;
    while (!*f && arch_now() < dl) {
        hal_net_pump();
        sys_check_timeouts();
        arch_relax();
    }
    return *f;
}

static bool parse_ipv4(const char *s, ip_addr_t *out) {
    if (!s) return false;
    uint32_t parts[4] = {0};
    int idx = 0, cur = -1;
    for (; *s && idx < 4; s++) {
        if (*s >= '0' && *s <= '9') {
            if (cur < 0) cur = 0;
            cur = cur * 10 + (*s - '0');
            if (cur > 255) return false;
        } else if (*s == '.') {
            if (cur < 0) return false;
            parts[idx++] = (uint32_t)cur;
            cur = -1;
        } else {
            return false;
        }
    }
    if (idx != 3 || cur < 0) return false;
    parts[3] = (uint32_t)cur;
    IP4_ADDR(out, parts[0], parts[1], parts[2], parts[3]);
    return true;
}

/* ---- HTTP ------------------------------------------------------------- */

static volatile bool g_tcp_done;
static volatile bool g_tcp_fail;
static int           g_tcp_status;
static bool          g_tcp_hdr_done;
static char          g_tcp_hdr[512];
static uint16_t      g_tcp_hdr_n;
static char          g_tcp_body[4096];
static uint16_t      g_tcp_body_n;
static const char   *g_tcp_path;

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK) { g_tcp_fail = true; g_tcp_done = true; return ERR_OK; }
    if (!p) { g_tcp_done = true; tcp_close(pcb); return ERR_OK; }
    char tmp[512];
    uint16_t n = p->tot_len;
    if (n > sizeof(tmp)) n = sizeof(tmp);
    pbuf_copy_partial(p, tmp, n, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    for (uint16_t i = 0; i < n; i++) {
        if (!g_tcp_hdr_done) {
            if (g_tcp_hdr_n < sizeof(g_tcp_hdr) - 1) g_tcp_hdr[g_tcp_hdr_n++] = tmp[i];
            if (g_tcp_hdr_n >= 4 &&
                g_tcp_hdr[g_tcp_hdr_n - 4] == '\r' &&
                g_tcp_hdr[g_tcp_hdr_n - 3] == '\n' &&
                g_tcp_hdr[g_tcp_hdr_n - 2] == '\r' &&
                g_tcp_hdr[g_tcp_hdr_n - 1] == '\n') {
                g_tcp_hdr_done = true;
                if (g_tcp_hdr_n >= 12) {
                    g_tcp_status = (g_tcp_hdr[9]  - '0') * 100
                                 + (g_tcp_hdr[10] - '0') * 10
                                 + (g_tcp_hdr[11] - '0');
                }
            }
        } else if (g_tcp_body_n < sizeof(g_tcp_body) - 1) {
            g_tcp_body[g_tcp_body_n++] = tmp[i];
        }
    }
    return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) { g_tcp_fail = true; g_tcp_done = true; return err; }
    char req[256];
    int rn = 0;
    const char *parts[] = {
        "GET ", g_tcp_path ? g_tcp_path : "/",
        " HTTP/1.1\r\nHost: canboot\r\nConnection: close\r\n\r\n", NULL
    };
    for (int i = 0; parts[i]; i++) {
        const char *q = parts[i];
        while (*q && rn < (int)sizeof(req)) req[rn++] = *q++;
    }
    err_t e = tcp_write(pcb, req, (u16_t)rn, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK) tcp_output(pcb);
    return e;
}

static void tcp_err_cb(void *arg, err_t err) {
    (void)arg; (void)err;
    g_tcp_fail = true; g_tcp_done = true;
}

static int http_get(const char *url, char *out_body, size_t out_cap, int *out_status) {
    struct url_parts p;
    if (parse_url(url, &p) != 0) return -1;
    ip_addr_t dst;
    if (!parse_ipv4(p.host, &dst)) return -1;

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return -1;

    g_tcp_done = false; g_tcp_fail = false;
    g_tcp_hdr_n = 0; g_tcp_hdr_done = false; g_tcp_status = 0;
    g_tcp_body_n = 0;
    g_tcp_path = p.path;

    tcp_recv(pcb, tcp_recv_cb);
    tcp_err(pcb, tcp_err_cb);
    if (tcp_connect(pcb, &dst, p.port, tcp_connected_cb) != ERR_OK) {
        tcp_abort(pcb);
        return -1;
    }
    if (!wait_flag(&g_tcp_done, 5000) || g_tcp_fail) return -1;
    size_t n = g_tcp_body_n < out_cap - 1 ? g_tcp_body_n : out_cap - 1;
    memcpy(out_body, g_tcp_body, n);
    out_body[n] = '\0';
    if (out_status) *out_status = g_tcp_status;
    return (int)n;
}

static int h_get(CandoVM *vm, int argc, CandoValue *args) {
    const char *url = libutil_arg_cstr_at(args, argc, 0);
    static char body[4096];
    int status = 0;
    int n = http_get(url, body, sizeof(body), &status);
    if (n < 0 || status != 200) { cando_vm_push(vm, cando_null()); return 1; }
    CandoString *s = cando_string_new(body, (uint32_t)n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int h_status(CandoVM *vm, int argc, CandoValue *args) {
    const char *url = libutil_arg_cstr_at(args, argc, 0);
    static char body[4096];
    int status = 0;
    int n = http_get(url, body, sizeof(body), &status);
    if (n < 0) status = 0;
    cando_vm_push(vm, cando_number((f64)status));
    return 1;
}

static const LibutilMethodEntry http_methods[] = {
    { "get",    h_get    },
    { "status", h_status },
};

void canboot_cando_open_httplib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, http_methods,
                             sizeof(http_methods) / sizeof(http_methods[0]));
    cando_vm_set_global(vm, "http", obj_val, true);
}

/* ---- HTTPS ------------------------------------------------------------ */

static mbedtls_entropy_context  g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt         g_ca;
static mbedtls_ssl_config       g_conf;
static mbedtls_ssl_context      g_ssl;
static struct canboot_lwip_bio  g_bio;
static char                     g_https_rx[8192];

static int https_get_url(const char *url, char *out_body, size_t out_cap,
                         int *out_status, const char *sni) {
    struct url_parts p;
    if (parse_url(url, &p) != 0) return -1;
    ip_addr_t dst;
    if (!parse_ipv4(p.host, &dst)) return -1;

    if (!sni) sni = "canboot-test";

    mbedtls_entropy_init(&g_ent);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_x509_crt_init(&g_ca);
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_ssl_init(&g_ssl);

    int n_out = -1;
    int status = 0;
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent,
                              (const unsigned char *)"canboot-https", 13) != 0) goto done;
    if (mbedtls_x509_crt_parse(&g_ca,
                               (const unsigned char *)canboot_test_ca_pem,
                               canboot_test_ca_pem_len) != 0) goto done;
    if (mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto done;
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_conf, &g_ca, NULL);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
    if (mbedtls_ssl_setup(&g_ssl, &g_conf) != 0) goto done;
    if (mbedtls_ssl_set_hostname(&g_ssl, sni) != 0) goto done;

    canboot_lwip_bio_init(&g_bio);
    if (canboot_lwip_bio_connect(&g_bio, &dst, p.port, 5000) != 0) goto done;
    mbedtls_ssl_set_bio(&g_ssl, &g_bio,
                        canboot_lwip_bio_send,
                        canboot_lwip_bio_recv, NULL);
    int rc;
    while ((rc = mbedtls_ssl_handshake(&g_ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    }
    if (mbedtls_ssl_get_verify_result(&g_ssl) != 0) goto done;

    char req[256]; int rn = 0;
    const char *parts[] = {
        "GET ", p.path, " HTTP/1.1\r\nHost: ", sni,
        "\r\nConnection: close\r\n\r\n", NULL
    };
    for (int i = 0; parts[i]; i++) {
        const char *q = parts[i];
        while (*q && rn < (int)sizeof(req) - 1) req[rn++] = *q++;
    }
    if (mbedtls_ssl_write(&g_ssl, (const unsigned char *)req, (size_t)rn) < 0) goto done;

    int total = 0;
    for (;;) {
        int r = mbedtls_ssl_read(&g_ssl, (unsigned char *)g_https_rx + total,
                                 sizeof(g_https_rx) - 1 - total);
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) break;
        total += r;
        if (total >= (int)sizeof(g_https_rx) - 1) break;
    }
    g_https_rx[total] = '\0';
    if (total >= 12 && g_https_rx[0] == 'H') {
        status = (g_https_rx[9] - '0') * 100
               + (g_https_rx[10] - '0') * 10
               + (g_https_rx[11] - '0');
    }
    const char *body = strstr(g_https_rx, "\r\n\r\n");
    body = body ? body + 4 : g_https_rx;
    int body_len = total - (int)(body - g_https_rx);
    if (body_len < 0) body_len = 0;
    if ((size_t)body_len > out_cap - 1) body_len = (int)(out_cap - 1);
    memcpy(out_body, body, (size_t)body_len);
    out_body[body_len] = '\0';
    n_out = body_len;
    mbedtls_ssl_close_notify(&g_ssl);

done:
    canboot_lwip_bio_close(&g_bio);
    mbedtls_ssl_free(&g_ssl);
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_x509_crt_free(&g_ca);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_ent);
    if (out_status) *out_status = status;
    return n_out;
}

static int s_get(CandoVM *vm, int argc, CandoValue *args) {
    const char *url = libutil_arg_cstr_at(args, argc, 0);
    const char *sni = libutil_arg_cstr_at(args, argc, 1);
    static char body[8192];
    int status = 0;
    int n = https_get_url(url, body, sizeof(body), &status, sni);
    if (n < 0 || status != 200) { cando_vm_push(vm, cando_null()); return 1; }
    CandoString *s = cando_string_new(body, (uint32_t)n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry https_methods[] = {
    { "get", s_get },
};

void canboot_cando_open_httpslib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, https_methods,
                             sizeof(https_methods) / sizeof(https_methods[0]));
    cando_vm_set_global(vm, "https", obj_val, true);
}
