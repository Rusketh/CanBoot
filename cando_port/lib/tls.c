/*
 * cando tls module - synchronous HTTPS GET backed by Mbed TLS + lwIP.
 *
 *   tls.httpsGet(host, port, path, [hostname])
 *     host     dotted-quad string ("10.0.2.2")
 *     port     numeric TCP port (default 443)
 *     path     URL path (default "/")
 *     hostname optional SNI / cert-CN, defaults to "canboot-test" so
 *              the bundled CA in tests/selftest/ca.c continues to verify
 *
 * Returns the response body as a string on a 200 with chain-validated
 * cert, or null on any failure (connect / handshake / status != 200).
 *
 * Reuses the m7 BIO + CA infrastructure so the trust anchor stays
 * pinned to tests/sidecars/tls/canboot-test.pem.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "lwip/sys.h"
#include "lwip/ip4_addr.h"

#include "lwip_bio.h"
#include "canboot_resolver.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

extern const char    canboot_test_ca_pem[];
extern const size_t  canboot_test_ca_pem_len;

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

static bool resolve_host(const char *s, ip_addr_t *out) {
    if (parse_ipv4(s, out)) return true;
    return s && canboot_dns_resolve(s, out, 5000) == 0;
}

/* Protocol negotiated by the most recent httpsGet ("TLSv1.3" / "TLSv1.2"),
 * exposed via tls.version() so scripts can confirm the handshake version. */
static const char *g_last_version = "";

/* Module-local TLS state. Reinitialised on every call so a failed
 * handshake doesn't poison subsequent attempts. */
static mbedtls_entropy_context  g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt         g_ca;
static mbedtls_ssl_config       g_conf;
static mbedtls_ssl_context      g_ssl;
static struct canboot_lwip_bio  g_bio;
static char                     g_rx[8192];

static int tls_https_get(CandoVM *vm, int argc, CandoValue *args) {
    const char *host = libutil_arg_cstr_at(args, argc, 0);
    uint16_t    port = (uint16_t)libutil_arg_num_at(args, argc, 1, 443);
    const char *path = libutil_arg_cstr_at(args, argc, 2);
    const char *sni  = libutil_arg_cstr_at(args, argc, 3);
    if (!path) path = "/";
    if (!sni)  sni  = "canboot-test";

    ip_addr_t dst;
    if (!host || !resolve_host(host, &dst)) { cando_vm_push(vm, cando_null()); return 1; }

    mbedtls_entropy_init(&g_ent);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_x509_crt_init(&g_ca);
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_ssl_init(&g_ssl);

    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent,
                              (const unsigned char *)"canboot-tls-lib", 16) != 0) goto fail;
    if (mbedtls_x509_crt_parse(&g_ca,
                               (const unsigned char *)canboot_test_ca_pem,
                               canboot_test_ca_pem_len) != 0) goto fail;
    if (mbedtls_ssl_config_defaults(&g_conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto fail;
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_conf, &g_ca, NULL);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);

    if (mbedtls_ssl_setup(&g_ssl, &g_conf) != 0) goto fail;
    if (mbedtls_ssl_set_hostname(&g_ssl, sni) != 0) goto fail;

    canboot_lwip_bio_init(&g_bio);
    if (canboot_lwip_bio_connect(&g_bio, &dst, port, 5000) != 0) goto fail;

    mbedtls_ssl_set_bio(&g_ssl, &g_bio,
                        canboot_lwip_bio_send,
                        canboot_lwip_bio_recv, NULL);

    int rc;
    while ((rc = mbedtls_ssl_handshake(&g_ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) goto fail;
    }
    if (mbedtls_ssl_get_verify_result(&g_ssl) != 0) goto fail;
    g_last_version = mbedtls_ssl_get_version(&g_ssl);

    /* Build + send GET request. */
    char req[256];
    int rn = 0;
    const char *parts[] = {
        "GET ", path, " HTTP/1.1\r\nHost: ", sni, "\r\nConnection: close\r\n\r\n", NULL
    };
    for (int i = 0; parts[i]; i++) {
        const char *q = parts[i];
        while (*q && rn < (int)sizeof(req) - 1) req[rn++] = *q++;
    }
    if (mbedtls_ssl_write(&g_ssl, (const unsigned char *)req, (size_t)rn) < 0) goto fail;

    /* Drain response into g_rx. */
    int total = 0;
    for (;;) {
        int n = mbedtls_ssl_read(&g_ssl,
                                 (unsigned char *)g_rx + total,
                                 sizeof(g_rx) - 1 - total);
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) break;
        total += n;
        if (total >= (int)sizeof(g_rx) - 1) break;
    }
    g_rx[total] = '\0';

    mbedtls_ssl_close_notify(&g_ssl);
    canboot_lwip_bio_close(&g_bio);

    /* Parse status from "HTTP/1.x NNN " and skip headers. */
    int status = (total >= 12 && g_rx[0] == 'H')
        ? (g_rx[9] - '0') * 100 + (g_rx[10] - '0') * 10 + (g_rx[11] - '0')
        : 0;
    const char *body = strstr(g_rx, "\r\n\r\n");
    body = body ? body + 4 : g_rx;
    int body_len = total - (int)(body - g_rx);
    if (body_len < 0) body_len = 0;

    mbedtls_ssl_free(&g_ssl);
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_x509_crt_free(&g_ca);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_ent);

    if (status != 200) { cando_vm_push(vm, cando_null()); return 1; }
    CandoString *s = cando_string_new(body, (size_t)body_len);
    cando_vm_push(vm, cando_string_value(s));
    return 1;

fail:
    canboot_lwip_bio_close(&g_bio);
    mbedtls_ssl_free(&g_ssl);
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_x509_crt_free(&g_ca);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_ent);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int tls_version(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    CandoString *s = cando_string_new(g_last_version,
                                      (uint32_t)strlen(g_last_version));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry tls_methods[] = {
    { "httpsGet", tls_https_get },
    { "version",  tls_version   },
};

void canboot_cando_open_tlslib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, tls_methods,
                             sizeof(tls_methods) / sizeof(tls_methods[0]));
    cando_vm_set_global(vm, "tls", obj_val, true);
}
