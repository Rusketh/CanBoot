/*
 * BSD-socket layer over lwIP's raw (NO_SYS) TCP API, so CanDo's real
 * socket / http libraries (which sit on lib/sockutil.c's POSIX socket
 * calls) run unmodified on canboot.
 *
 * lwIP NO_SYS is single-flow: all core calls must be serialised. We take
 * a global netcore lock (preempt-disabled, SMP-safe) around every lwIP
 * critical section, and never hold it across a blocking wait - blocking
 * ops loop { lock; pump lwIP; check; unlock; yield } so several threads
 * cooperatively share the one lwIP flow. lwIP's RX/connect/sent
 * callbacks fire synchronously inside the pump (same thread, lock held),
 * so they touch the socket table without extra locking.
 *
 * Socket fds live in a dedicated range (>= SOCK_FD_BASE) so they don't
 * collide with the picolibc file fds; close() in the picolibc port routes
 * them here via the weak canboot_socket_close hook.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/time.h>   /* struct timeval (SO_RCVTIMEO/SO_SNDTIMEO) */
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/timeouts.h"

#include "hal/net.h"
#include "sync/spinlock.h"
#include "sync/cpu.h"
#include "sched/sched.h"
#include "canboot_resolver.h"

#define SOCK_FD_BASE 1000
#define NSOCK        16
#define RXCAP        32768u
#define BACKLOG_MAX  8

enum { ST_FREE = 0, ST_NEW, ST_CONNECTING, ST_CONNECTED, ST_LISTEN, ST_CLOSED, ST_ERR };

struct nsock {
    int               used;
    int               state;
    struct tcp_pcb   *pcb;
    uint8_t           rx[RXCAP];
    volatile uint32_t rxh, rxt;       /* ring: count = rxt - rxh             */
    volatile int      eof;            /* peer FIN received                   */
    volatile int      err;            /* lwIP error latched                  */
    int               nonblock;
    int               timeout_ms;
    int               domain;
    /* listener accept backlog: indices of fully-wired connection slots
     * (recv_cb already attached in accept_cb) awaiting accept().            */
    int               backlog[BACKLOG_MAX];
    volatile int      bl_count;
    uint16_t          local_port;
    uint16_t          peer_port;
    ip_addr_t         peer_ip;
};

static struct nsock g_socks[NSOCK];
static spinlock_t   g_netlock = SPINLOCK_INITIALIZER;

static void net_enter(void) { canboot_preempt_disable(); spin_lock(&g_netlock); }
static void net_leave(void) { spin_unlock(&g_netlock); canboot_preempt_enable(); }

static struct nsock *sock_of(int fd) {
    int i = fd - SOCK_FD_BASE;
    if (i < 0 || i >= NSOCK || !g_socks[i].used) return NULL;
    return &g_socks[i];
}

/* ---- millisecond clock (lwIP sys_now) --------------------------------- */
extern uint32_t sys_now(void);

/* Pump lwIP once under the lock, then yield so peers progress. Socket
 * I/O is a blocking point, so drop the VM GIL across the pump+yield: a
 * thread waiting on the network must not keep other VM threads (e.g. a
 * server's connection handlers, or the accept loop) from running. lwIP
 * is serialised separately by the netcore lock, not the GIL. */
static void pump_yield(void) {
    int had_gil = canboot_gil_owned_by_current();
    if (had_gil) canboot_gil_release();
    net_enter();
    hal_net_pump();
    sys_check_timeouts();
    net_leave();
    canboot_sched_yield();
    if (had_gil) canboot_gil_acquire();
}

/* ---- lwIP raw callbacks ----------------------------------------------- */
static void err_cb(void *arg, err_t e) {
    struct nsock *s = (struct nsock *)arg;
    if (!s) return;
    s->err = (int)e;
    s->pcb = NULL;            /* lwIP frees the pcb before calling err_cb */
    if (s->state == ST_CONNECTING) s->state = ST_ERR;
}

static err_t connected_cb(void *arg, struct tcp_pcb *pcb, err_t e) {
    struct nsock *s = (struct nsock *)arg;
    (void)pcb;
    if (!s) return ERR_OK;
    if (e != ERR_OK) { s->err = (int)e; s->state = ST_ERR; return e; }
    s->state = ST_CONNECTED;
    return ERR_OK;
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t e) {
    struct nsock *s = (struct nsock *)arg;
    if (!s) { if (p) pbuf_free(p); return ERR_OK; }
    if (e != ERR_OK) { s->err = (int)e; if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { s->eof = 1; return ERR_OK; }       /* clean close */
    uint16_t off = 0;
    while (off < p->tot_len) {
        uint32_t used = s->rxt - s->rxh;
        uint32_t space = RXCAP - used;
        if (space == 0) break;                   /* ring full: stop ACKing */
        uint32_t widx = s->rxt % RXCAP;
        uint32_t chunk = RXCAP - widx;           /* to ring end */
        if (chunk > space) chunk = space;
        uint32_t want = p->tot_len - off;
        if (chunk > want) chunk = want;
        pbuf_copy_partial(p, s->rx + widx, (uint16_t)chunk, off);
        s->rxt += chunk;
        off += chunk;
    }
    tcp_recved(pcb, off);                         /* ACK what we buffered */
    pbuf_free(p);
    return ERR_OK;
}

/* Runs inside hal_net_pump with the netcore lock already held (a pump is
 * always entered via net_enter), so it touches g_socks directly. We wire
 * the connection's recv/err callbacks HERE, the instant lwIP hands us the
 * pcb, rather than in accept(): a client that sends immediately after the
 * handshake would otherwise have its first segment arrive before accept()
 * attached recv_cb, and lwIP would refuse/stall it. */
static err_t accept_cb(void *arg, struct tcp_pcb *newpcb, err_t e) {
    struct nsock *ls = (struct nsock *)arg;
    if (!ls || e != ERR_OK || !newpcb) return ERR_VAL;
    if (ls->bl_count >= BACKLOG_MAX) return ERR_MEM;   /* drop: backlog full */
    int idx = -1;
    for (int i = 0; i < NSOCK; i++) if (!g_socks[i].used) { idx = i; break; }
    if (idx < 0) return ERR_MEM;                       /* pool full: refuse */
    struct nsock *c = &g_socks[idx];
    memset(c, 0, sizeof(*c));
    c->used = 1; c->state = ST_CONNECTED; c->pcb = newpcb; c->domain = AF_INET;
    c->peer_port = newpcb->remote_port;
    c->peer_ip   = newpcb->remote_ip;
    tcp_arg(newpcb, c);
    tcp_recv(newpcb, recv_cb);
    tcp_err(newpcb, err_cb);
    ls->backlog[ls->bl_count++] = idx;
    return ERR_OK;
}

/* ---- socket() / connect() / send() / recv() / close() ----------------- */

int socket(int domain, int type, int protocol) {
    (void)protocol;
    if (type != SOCK_STREAM) { errno = EINVAL; return -1; }
    net_enter();
    int idx = -1;
    for (int i = 0; i < NSOCK; i++) if (!g_socks[i].used) { idx = i; break; }
    if (idx < 0) { net_leave(); errno = EMFILE; return -1; }
    struct nsock *s = &g_socks[idx];
    memset(s, 0, sizeof(*s));
    s->used = 1; s->state = ST_NEW; s->domain = domain ? domain : AF_INET;
    s->timeout_ms = 0;
    net_leave();
    return SOCK_FD_BASE + idx;
}

static void sock_release(struct nsock *s) {
    if (s->pcb) {
        tcp_arg(s->pcb, NULL);
        tcp_recv(s->pcb, NULL);
        tcp_err(s->pcb, NULL);
        tcp_sent(s->pcb, NULL);
        if (tcp_close(s->pcb) != ERR_OK) tcp_abort(s->pcb);
        s->pcb = NULL;
    }
    for (int i = 0; i < s->bl_count; i++)
        if (s->backlog[i]) tcp_abort(s->backlog[i]);
    s->bl_count = 0;
    s->used = 0; s->state = ST_FREE;
}

int canboot_socket_is(int fd) { return fd >= SOCK_FD_BASE && fd < SOCK_FD_BASE + NSOCK; }

int canboot_socket_close(int fd) {
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    net_enter();
    sock_release(s);
    net_leave();
    return 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)len;
    struct nsock *s = sock_of(fd);
    if (!s || !addr) { errno = EBADF; return -1; }
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    ip_addr_t ip;
    IP_ADDR4(&ip, (sin->sin_addr.s_addr) & 0xFF,
             (sin->sin_addr.s_addr >> 8) & 0xFF,
             (sin->sin_addr.s_addr >> 16) & 0xFF,
             (sin->sin_addr.s_addr >> 24) & 0xFF);
    uint16_t port = ntohs(sin->sin_port);

    net_enter();
    s->pcb = tcp_new();
    if (!s->pcb) { net_leave(); errno = ENOBUFS; return -1; }
    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, recv_cb);
    tcp_err(s->pcb, err_cb);
    s->state = ST_CONNECTING;
    s->peer_ip = ip; s->peer_port = port;
    err_t e = tcp_connect(s->pcb, &ip, port, connected_cb);
    net_leave();
    if (e != ERR_OK) { errno = EHOSTUNREACH; return -1; }

    uint32_t start = sys_now();
    uint32_t tmo = s->timeout_ms > 0 ? (uint32_t)s->timeout_ms : 15000;
    while (s->state == ST_CONNECTING && (uint32_t)(sys_now() - start) < tmo)
        pump_yield();
    if (s->state == ST_CONNECTED) return 0;
    errno = (s->state == ST_ERR) ? ECONNREFUSED : ETIMEDOUT;
    return -1;
}

long send(int fd, const void *buf, size_t len, int flags) {
    (void)flags;
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    uint32_t start = sys_now();
    uint32_t tmo = s->timeout_ms > 0 ? (uint32_t)s->timeout_ms : 15000;
    while (sent < len) {
        if (s->err || !s->pcb) { errno = EPIPE; return sent ? (long)sent : -1; }
        net_enter();
        uint32_t sb = s->pcb ? tcp_sndbuf(s->pcb) : 0;
        size_t n = len - sent;
        if (n > sb) n = sb;
        err_t e = ERR_OK;
        if (n > 0) {
            e = tcp_write(s->pcb, p + sent, (uint16_t)n, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) { tcp_output(s->pcb); sent += n; }
        }
        net_leave();
        if (e != ERR_OK && e != ERR_MEM) { errno = EIO; return sent ? (long)sent : -1; }
        if (n == 0 || e == ERR_MEM) {
            if ((uint32_t)(sys_now() - start) >= tmo) { errno = EAGAIN; return sent ? (long)sent : -1; }
            pump_yield();
        }
    }
    return (long)sent;
}

long recv(int fd, void *buf, size_t len, int flags) {
    (void)flags;
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    uint32_t start = sys_now();
    uint32_t tmo = s->timeout_ms > 0 ? (uint32_t)s->timeout_ms : 15000;
    for (;;) {
        net_enter();
        uint32_t avail = s->rxt - s->rxh;
        if (avail > 0) {
            uint32_t n = avail < len ? avail : (uint32_t)len;
            uint32_t ridx = s->rxh % RXCAP;
            uint32_t chunk = RXCAP - ridx;
            if (chunk > n) chunk = n;
            memcpy(buf, s->rx + ridx, chunk);
            if (chunk < n) memcpy((uint8_t *)buf + chunk, s->rx, n - chunk);
            s->rxh += n;
            net_leave();
            return (long)n;
        }
        int eof = s->eof, err = s->err;
        net_leave();
        if (eof) return 0;
        if (err) { errno = ECONNRESET; return -1; }
        if (s->nonblock) { errno = EAGAIN; return -1; }
        if ((uint32_t)(sys_now() - start) >= tmo) { errno = EAGAIN; return -1; }
        pump_yield();
    }
}

int shutdown(int fd, int how) {
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    net_enter();
    if (s->pcb) tcp_shutdown(s->pcb, (how == SHUT_RD || how == SHUT_RDWR),
                             (how == SHUT_WR || how == SHUT_RDWR));
    net_leave();
    return 0;
}

/* ---- server: bind / listen / accept ----------------------------------- */
int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)len;
    struct nsock *s = sock_of(fd);
    if (!s || !addr) { errno = EBADF; return -1; }
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    s->local_port = ntohs(sin->sin_port);
    return 0;
}

int listen(int fd, int backlog) {
    (void)backlog;
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    net_enter();
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { net_leave(); errno = ENOBUFS; return -1; }
    if (tcp_bind(pcb, IP_ADDR_ANY, s->local_port) != ERR_OK) {
        tcp_abort(pcb); net_leave(); errno = EADDRINUSE; return -1;
    }
    struct tcp_pcb *lp = tcp_listen(pcb);
    if (!lp) { tcp_abort(pcb); net_leave(); errno = EADDRINUSE; return -1; }
    s->pcb = lp;
    s->state = ST_LISTEN;
    tcp_arg(lp, s);
    tcp_accept(lp, accept_cb);
    net_leave();
    return 0;
}

int accept(int fd, struct sockaddr *addr, socklen_t *alen) {
    struct nsock *s = sock_of(fd);
    if (!s || s->state != ST_LISTEN) { errno = EBADF; return -1; }
    uint32_t start = sys_now();
    uint32_t tmo = s->timeout_ms > 0 ? (uint32_t)s->timeout_ms : 0;  /* 0 = wait forever */
    for (;;) {
        net_enter();
        if (s->bl_count > 0) {
            int idx = s->backlog[0];          /* slot already wired in accept_cb */
            for (int i = 1; i < s->bl_count; i++) s->backlog[i - 1] = s->backlog[i];
            s->bl_count--;
            struct nsock *c = &g_socks[idx];
            if (addr && alen && *alen >= sizeof(struct sockaddr_in)) {
                struct sockaddr_in *sin = (struct sockaddr_in *)addr;
                memset(sin, 0, sizeof(*sin));
                sin->sin_family = AF_INET;
                sin->sin_port = htons(c->peer_port);
                sin->sin_addr.s_addr = ip_addr_get_ip4_u32(&c->peer_ip);
                *alen = sizeof(*sin);
            }
            net_leave();
            return SOCK_FD_BASE + idx;
        }
        net_leave();
        if (tmo && (uint32_t)(sys_now() - start) >= tmo) { errno = EAGAIN; return -1; }
        pump_yield();
    }
}

/* ---- options + address introspection ---------------------------------- */
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)level; (void)optlen;
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    if ((optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) && optval) {
        const struct timeval *tv = (const struct timeval *)optval;
        s->timeout_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    }
    return 0;   /* REUSEADDR / NODELAY / KEEPALIVE / buffers: accepted no-ops */
}

int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    (void)level;
    struct nsock *s = sock_of(fd);
    if (!s || !optval || !optlen) { errno = EBADF; return -1; }
    if (optname == SO_ERROR && *optlen >= sizeof(int)) {
        *(int *)optval = s->err ? ECONNRESET : 0;
        *optlen = sizeof(int);
    }
    return 0;
}

static void fill_sin(struct sockaddr *out, socklen_t *outlen,
                     uint32_t ip4, uint16_t port) {
    if (!out || !outlen || *outlen < sizeof(struct sockaddr_in)) return;
    struct sockaddr_in *sin = (struct sockaddr_in *)out;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = ip4;
    *outlen = sizeof(*sin);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    fill_sin(addr, len, s->pcb ? ip_addr_get_ip4_u32(&s->pcb->local_ip) : 0,
             s->pcb ? s->pcb->local_port : s->local_port);
    return 0;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    struct nsock *s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    fill_sin(addr, len, ip_addr_get_ip4_u32(&s->peer_ip), s->peer_port);
    return 0;
}

/* ---- name resolution -------------------------------------------------- */
const char *gai_strerror(int e) { (void)e; return "getaddrinfo error"; }

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *n = res->ai_next;
        if (res->ai_addr) free(res->ai_addr);
        free(res);
        res = n;
    }
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    (void)hints;
    if (!res) return EAI_FAIL;
    *res = NULL;
    int port = service ? atoi(service) : 0;

    ip_addr_t ip;
    if (!node || node[0] == '\0') {
        IP_ADDR4(&ip, 0, 0, 0, 0);              /* AI_PASSIVE: any */
    } else if (canboot_dns_resolve(node, &ip, 5000) != 0) {
        return EAI_NONAME;
    }

    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(*ai));
    struct sockaddr_in *sin = (struct sockaddr_in *)calloc(1, sizeof(*sin));
    if (!ai || !sin) { free(ai); free(sin); return EAI_MEMORY; }
    sin->sin_family = AF_INET;
    sin->sin_port = htons((uint16_t)port);
    sin->sin_addr.s_addr = ip_addr_get_ip4_u32(&ip);
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_protocol = IPPROTO_TCP;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    *res = ai;
    return 0;
}

/* inet_pton lives in net/mbedtls_port/inet_pton.c (handles v4 + v6). */

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (!src || !dst) return NULL;
    const unsigned char *b = (const unsigned char *)src;   /* network order */
    if (af == AF_INET) {
        int n = snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return (n > 0 && (socklen_t)n < size) ? dst : NULL;
    }
    if (af == AF_INET6) {
        /* Uncompressed eight-group form; sufficient for numeric display. */
        int n = snprintf(dst, size, "%x:%x:%x:%x:%x:%x:%x:%x",
                         (b[0] << 8) | b[1],   (b[2] << 8) | b[3],
                         (b[4] << 8) | b[5],   (b[6] << 8) | b[7],
                         (b[8] << 8) | b[9],   (b[10] << 8) | b[11],
                         (b[12] << 8) | b[13], (b[14] << 8) | b[15]);
        return (n > 0 && (socklen_t)n < size) ? dst : NULL;
    }
    return NULL;
}

/* Numeric-only getnameinfo: cando's sockutil always passes NI_NUMERICHOST/
 * NI_NUMERICSERV, so there is no reverse-DNS path to honour. */
int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)salen; (void)flags;
    if (!sa || sa->sa_family != AF_INET) return EAI_FAMILY;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    if (host && hostlen) {
        const unsigned char *b = (const unsigned char *)&sin->sin_addr.s_addr;
        int n = snprintf(host, hostlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        if (n < 0 || (socklen_t)n >= hostlen) return EAI_MEMORY;
    }
    if (serv && servlen) {
        int n = snprintf(serv, servlen, "%u", ntohs(sin->sin_port));
        if (n < 0 || (socklen_t)n >= servlen) return EAI_MEMORY;
    }
    return 0;
}
