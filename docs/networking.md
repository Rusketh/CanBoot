# Networking stack

```
   cando script
        │
        ▼
   net.* / http.* / https.* / tls.* / url.*       (cando bindings)
        │
        ▼
   lwIP raw API (tcp_*, udp_*, dns_*)
        │
        ▼
   Mbed TLS (over an lwIP-backed BIO)             (for TLS-protected calls)
        │
        ▼
   netif      hal_net_pump                        (HAL surface)
        │           │
        ▼           ▼
   virtio-net-pci driver
        │
        ▼
   Hardware (or QEMU SLIRP)
```

## lwIP

Vendored at `vendor/lwip` (lwip-tcpip/lwip 2.2.1). Built in `NO_SYS=1`
mode — no threads, no OS layer, raw API only. The port lives in
`net/lwip_port/`:

- `net/lwip_port/include/lwipopts.h` — feature toggles. ARP, IPv4,
  UDP, TCP, DHCP, ICMP, etc. enabled; IPv6 disabled for now.
- `net/lwip_port/sys_arch.c` — provides `sys_now()` backed by the
  TSC-calibrated clock from milestone 6.
- `net/lwip_port/inet_pton.c` — inet_pton(AF_INET, ...) shim.

The virtio-net driver feeds lwIP via `netif_add` + `etharp_output`;
inbound packets land via `tcpip_input` from inside `hal_net_pump`.

## Mbed TLS

Vendored at `vendor/mbedtls` (Mbed-TLS/mbedtls LTS 3.6.6). Built via
`add_subdirectory(EXCLUDE_FROM_ALL)` with a canboot user config
(`net/mbedtls_port/include/canboot_mbedtls_user_config.h`) that strips
POSIX dependencies.

Port:

- `net/mbedtls_port/entropy.c` — RDSEED/RDRAND with CPUID guard,
  TSC-jitter mixer fallback.
- `net/mbedtls_port/lwip_bio.c` — synchronous send/recv shim around
  lwIP's raw TCP API. Mbed TLS expects blocking I/O; lwIP's raw API
  is callback-driven, so this layer parks on a flag set by the lwIP
  callbacks while cooperatively pumping.
- `net/mbedtls_port/timing.c` — TSC-backed `mbedtls_timing_*`.

## DHCP at boot

`kernel/m6_nettest.c` is what kicks DHCP. The netif starts in
`netif_set_default(&g_netif); netif_set_up(&g_netif);` then
`dhcp_start(&g_netif)`. The pump runs `dhcp_fine_tmr` / `dhcp_coarse_tmr`
at the spec'd cadences via lwIP's `sys_check_timeouts()`.

By the time `kmain` reaches the cando milestone, the netif typically
has a lease and the cando `net.*` calls Just Work.

## TLS handshake flow

```
cando: tls.httpsGet("https://10.0.2.2:8443/health")
              │
              ▼
cando_tls_lib.c::t_https_get
              │
              ▼
mbedtls_ssl_setup + mbedtls_ssl_set_hostname + mbedtls_ssl_set_bio
              │
              ▼
mbedtls_ssl_handshake  ── lwip_bio_send/recv ── lwIP raw TCP
              │
              ▼
mbedtls_ssl_write (HTTP GET) + mbedtls_ssl_read (response)
              │
              ▼
HTTP body returned to cando
```

On a second connection to the same host:port, the session ticket
from the first handshake is reused — handshake#2 is ~30× faster
than #1 (`milestone 7: session resumption ok (hs1=80697 us hs2=2152 us)`).

## CA pinning

The handshake validates against the canboot test CA only — a single
self-signed cert at `tests/sidecars/tls/canboot-test.pem`, embedded
in the kernel via `kernel/canboot_test_ca.c`. Regenerate via
`scripts/embed-test-ca.sh`.

To trust additional CAs in a custom build, append them to the source
PEM and re-run the embed script. Multiple PEM-encoded certs in one
file is the standard concatenation form.

## QEMU sidecar servers

The smoke tests spawn three Python sidecar servers on the host to
exercise the network paths:

| Sidecar | Port | What |
|---------|------|------|
| `tests/sidecars/udp_echo.py`    | 7777 | UDP echo |
| `tests/sidecars/http_hello.py`  | 8080 | HTTP serves "canboot-hello" |
| `tests/sidecars/https_secure.py`| 8443 | HTTPS serves "canboot-secure" with the test CA |

QEMU SLIRP forwards `10.0.2.2:NNNN` (from the guest) to the host's
`127.0.0.1:NNNN`. lwIP doesn't see SLIRP — it's an Ethernet driver
boundary above it.

## Where to read source

| Concern | File |
|---------|------|
| lwIP integration  | `net/lwip_port/*` |
| Mbed TLS integration | `net/mbedtls_port/*` |
| virtio-net driver | `hal/net/virtio_net.c` |
| cando bindings    | `cando_port/cando_net_lib.c`, `cando_port/cando_http_lib.c`, `cando_port/cando_https_lib.c`, `cando_port/cando_tls_lib.c` |
| DHCP + initial net bring-up | `kernel/m6_nettest.c` |
| TLS bring-up + sample fetch | `kernel/m7_tlstest.c` |
