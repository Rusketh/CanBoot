/*
 * cando net module - synchronous wrappers around the lwIP raw API.
 *
 *   net.udpEcho(host, port, msg)        -> response string or null
 *   net.httpGet(host, port, path)       -> body string or null
 *
 * `host` is a dotted-quad string ("10.0.2.2"); name resolution lands
 * once DNS is wired. Each call drives the lwIP pump until it
 * completes or times out (5s by default). Synchronous because cando
 * is single-threaded.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "lwip/sys.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"

#include "hal/net.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

extern uint64_t canboot_tsc_hz(void);

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

/* ---- UDP echo --------------------------------------------------------- */

static volatile bool g_udp_done;
static char          g_udp_rx[1024];
static uint16_t      g_udp_rxlen;

static void udp_rx_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *from, u16_t port) {
    (void)arg; (void)pcb; (void)from; (void)port;
    if (!p) return;
    uint16_t n = p->tot_len;
    if (n > sizeof(g_udp_rx)) n = sizeof(g_udp_rx);
    pbuf_copy_partial(p, g_udp_rx, n, 0);
    g_udp_rxlen = n;
    g_udp_done = true;
    pbuf_free(p);
}

static int n_udp_echo(CandoVM *vm, int argc, CandoValue *args) {
    const char *host = libutil_arg_cstr_at(args, argc, 0);
    uint16_t    port = (uint16_t)libutil_arg_num_at(args, argc, 1, 0);
    const char *msg  = libutil_arg_cstr_at(args, argc, 2);
    if (!host || !msg) { cando_vm_push(vm, cando_null()); return 1; }
    ip_addr_t dst;
    if (!parse_ipv4(host, &dst)) { cando_vm_push(vm, cando_null()); return 1; }

    struct udp_pcb *pcb = udp_new();
    if (!pcb) { cando_vm_push(vm, cando_null()); return 1; }

    g_udp_done = false;
    g_udp_rxlen = 0;
    udp_recv(pcb, udp_rx_cb, NULL);

    uint16_t mlen = (uint16_t)strlen(msg);
    struct pbuf *tx = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    if (tx) {
        pbuf_take(tx, msg, mlen);
        udp_sendto(pcb, tx, &dst, port);
        pbuf_free(tx);
    }

    bool ok = wait_flag(&g_udp_done, 5000);
    udp_remove(pcb);
    if (!ok) { cando_vm_push(vm, cando_null()); return 1; }

    CandoString *s = cando_string_new(g_udp_rx, g_udp_rxlen);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* ---- TCP HTTP GET ----------------------------------------------------- */

static volatile bool g_tcp_done;
static volatile bool g_tcp_fail;
static int           g_tcp_status;
static bool          g_tcp_hdr_done;
static char          g_tcp_hdr[512];
static uint16_t      g_tcp_hdr_n;
static char          g_tcp_body[4096];
static uint16_t      g_tcp_body_n;

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

static const char *g_tcp_path;
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) { g_tcp_fail = true; g_tcp_done = true; return err; }
    char req[256];
    int rn = 0;
    const char *p = g_tcp_path ? g_tcp_path : "/";
    const char *parts[] = {
        "GET ", p, " HTTP/1.1\r\nHost: canboot\r\nConnection: close\r\n\r\n", NULL
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

static int n_http_get(CandoVM *vm, int argc, CandoValue *args) {
    const char *host = libutil_arg_cstr_at(args, argc, 0);
    uint16_t    port = (uint16_t)libutil_arg_num_at(args, argc, 1, 80);
    const char *path = libutil_arg_cstr_at(args, argc, 2);

    ip_addr_t dst;
    if (!host || !parse_ipv4(host, &dst)) { cando_vm_push(vm, cando_null()); return 1; }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { cando_vm_push(vm, cando_null()); return 1; }

    g_tcp_done = false; g_tcp_fail = false;
    g_tcp_hdr_n = 0; g_tcp_hdr_done = false; g_tcp_status = 0;
    g_tcp_body_n = 0;
    g_tcp_path = path;

    tcp_recv(pcb, tcp_recv_cb);
    tcp_err(pcb, tcp_err_cb);

    if (tcp_connect(pcb, &dst, port, tcp_connected_cb) != ERR_OK) {
        tcp_abort(pcb);
        cando_vm_push(vm, cando_null());
        return 1;
    }

    bool ok = wait_flag(&g_tcp_done, 5000);
    if (!ok || g_tcp_fail || g_tcp_status != 200) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    g_tcp_body[g_tcp_body_n] = '\0';
    CandoString *s = cando_string_new(g_tcp_body, g_tcp_body_n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* ---- DNS / address resolution ----------------------------------------- */
/*
 * net.lookup(host) -> array of IP strings
 *
 * Drop-in for vendor/cando/source/lib/net.c's `net.lookup`. Host CanDo
 * delegates to POSIX getaddrinfo; on canboot we don't have a resolver
 * yet (lwipopts.h has LWIP_DNS=0 — enabling it pulls in the DNS module
 * + a sys-timer dependency that hasn't been wired). For now we accept
 * dotted-quad IPv4 literals only:
 *
 *   - input parses as IPv4 -> single-element array of the input
 *   - anything else        -> empty array (matches CanDo's failure shape;
 *                             does NOT throw)
 *
 * Scripts that call this with hostnames get the same observable result
 * they would under host CanDo when getaddrinfo fails: an empty array.
 * Real DNS lookup lands when LWIP_DNS is enabled in a subsequent PR.
 */
static int n_lookup(CandoVM *vm, int argc, CandoValue *args) {
    const char *host = libutil_arg_cstr_at(args, argc, 0);

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr_obj = cando_bridge_resolve(vm, cando_as_handle(arr_val));

    ip_addr_t addr;
    if (host && parse_ipv4(host, &addr)) {
        CdoString *s = cdo_string_new(host, (uint32_t)strlen(host));
        cdo_array_push(arr_obj, cdo_string_value(s));
        cdo_string_release(s);
    }

    cando_vm_push(vm, arr_val);
    return 1;
}

static const LibutilMethodEntry net_methods[] = {
    /* CanDo drop-in surface. */
    { "lookup",  n_lookup   },

    /* CanBoot-specific extensions retained until full socket / http
     * surfaces land (workstreams 9.socket/9.http). Removed once the
     * new APIs are in place and init.cdo / smoke tests are migrated. */
    { "udpEcho", n_udp_echo },
    { "httpGet", n_http_get },
};

void canboot_cando_open_netlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, net_methods,
                             sizeof(net_methods) / sizeof(net_methods[0]));
    cando_vm_set_global(vm, "net", obj_val, true);
}
