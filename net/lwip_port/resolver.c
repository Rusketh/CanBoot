/*
 * Synchronous DNS resolution for the NO_SYS lwIP build. lwIP's
 * dns_gethostbyname is asynchronous (the answer arrives via a callback
 * when the UDP response is pumped in), so we spin the net pump + lwIP
 * timer wheel until the callback fires or we time out. cando is
 * single-threaded, so this is the only flow inside lwIP.
 */

#include <stddef.h>

#include "lwip/dns.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"

#include "hal/net.h"
#include "canboot_resolver.h"

struct dns_wait {
    volatile int done;
    volatile int ok;
    ip_addr_t    addr;
};

static void dns_done(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    struct dns_wait *w = (struct dns_wait *)arg;
    if (ipaddr) { w->addr = *ipaddr; w->ok = 1; }
    w->done = 1;
}

int canboot_dns_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms) {
    if (!host || !out) return -1;

    struct dns_wait w;
    w.done = 0;
    w.ok = 0;
    ip_addr_set_zero(&w.addr);

    err_t e = dns_gethostbyname(host, &w.addr, dns_done, &w);
    if (e == ERR_OK) { *out = w.addr; return 0; }     /* already cached */
    if (e != ERR_INPROGRESS) return -1;

    u32_t start = sys_now();
    while (!w.done && (u32_t)(sys_now() - start) < timeout_ms) {
        hal_net_pump();
        sys_check_timeouts();
    }
    if (w.ok) { *out = w.addr; return 0; }
    return -1;
}
