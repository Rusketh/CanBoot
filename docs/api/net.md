# net — networking

UDP, HTTP (cleartext and TLS), URL parsing. All calls block until they
get a response (or the timeout fires); the network stack is pumped
cooperatively from `time.ms` / `time.sleep` / `input.waitKey`, so
audio + input keep working during network calls.

- [`net`](#net) — UDP echo + HTTP GET over lwIP
- [`http`](#http) — cleartext HTTP GET (URL form)
- [`https`](#https) — HTTPS GET via Mbed TLS
- [`tls`](#tls) — TLS-protected fetch (symmetric naming with `net.*`)
- [`url`](#url) — URL parser

---

## net

Convenience socket-style helpers wrapping lwIP.

### `net.udpEcho(host, port, payload) -> string|null`

Send a UDP datagram to `host:port` and wait for a reply. `host` is
either a dotted-quad IP literal (`"10.0.2.2"`) or a hostname
(DNS-resolved via lwIP). Returns the reply bytes, or `null` on
timeout.

```cdo
VAR reply = net.udpEcho("10.0.2.2", 7777, "ping");
```

### `net.httpGet(url) -> string|null`

Issue an HTTP GET to the given URL. Returns the response body. `null`
on connection failure, non-2xx response, or timeout.

```cdo
VAR body = net.httpGet("http://10.0.2.2:8080/hello");
print(body);   // -> "canboot-hello"
```

URL parsing is naive — `host:port/path` form is the safe shape.

### Behaviour

- DHCP is run by the kernel at boot; by the time `/init.cdo` gets to
  `net.*`, the netif is up at whatever lease SLIRP / the host DHCP
  server handed out.
- Default timeouts: ~5 seconds for HTTP, ~3 seconds for UDP echo.
  Both block on lwIP's `select`-equivalent path.
- Single virtio-net device only. The HAL surface allows multiple, but
  cando exposes only the primary.

---

## http

URL-aware HTTP GET helper. Same wire path as [`net.httpGet`](#net)
but reads more naturally next to `https.get` since both take a full
URL.

### `http.get(url) -> string|null`

Fetch the body at `url` (must be `http://...`). Returns the response
body, or `null` on connection failure / non-2xx response / timeout.

```cdo
http.get("http://10.0.2.2:8080/hello")   // -> "canboot-hello"
```

For HTTPS, use [`https.get`](#https). For UDP echo, see
[`net.udpEcho`](#net).

---

## https

### `https.get(url) -> string|null`

Fetch the body at `url` (must be `https://...`). Same TLS pipeline as
[`tls.httpsGet`](#tls); differs only in name. Returns the response
body, or `null` on TLS handshake failure / HTTP error / timeout.

```cdo
https.get("https://10.0.2.2:8443/health")   // -> "canboot-secure"
```

CA pinning: the canboot test CA at
`tests/selftest/ca/canboot-test.pem` is embedded at build time. Add
your own CA via `tests/selftest/embed-test-ca.sh`.

---

## tls

Direct entry point for fetching HTTPS resources. Same underlying
TLS pipeline as [`https.*`](#https); the difference is that `tls.*`
exists for symmetry with `net.*` and reads more naturally when the
script's intent is "raw protected fetch" rather than "I want this
URL's body."

### `tls.httpsGet(url) -> string|null`

Fetch the body at the given `https://` URL using TLS 1.2. Uses the
canboot test CA embedded at build time to validate the chain. Returns
the response body, or `null` on TLS handshake failure, HTTP error, or
timeout.

```cdo
VAR body = tls.httpsGet("https://10.0.2.2:8443/health");
```

The handshake auto-resumes via session tickets on repeat calls to the
same host:port — second connection lands ~30× faster than the first.

### Behaviour

- **CA pinning**: only certificates issued under the canboot test CA
  validate. Production CAs (Let's Encrypt etc.) aren't trusted in the
  current build — they'd need to be embedded the same way the test
  CA is.
- **Cipher suite**: TLS 1.2 ECDHE-RSA-AES256-GCM-SHA384 (handshake
  default).
- **Entropy source**: RDSEED / RDRAND on x86_64 when available
  (CPUID-gated to skip `qemu64` which lacks the bit), falling back to
  a TSC-jitter mixer.
- See [`../networking.md`](../networking.md) for the full TLS
  pipeline.

---

## url

Lightweight URL parser. Splits a `scheme://host[:port]/path` string
into named fields.

### `url.scheme(url) -> string`

Lowercase scheme. `"http"`, `"https"`, etc. Empty string if missing.

```cdo
url.scheme("https://10.0.2.2:8443/health")   // -> "https"
```

### `url.host(url) -> string`

Host part. IP literal or hostname. Empty string if missing.

```cdo
url.host("https://10.0.2.2:8443/health")     // -> "10.0.2.2"
```

### `url.port(url) -> number`

Port number, or the scheme default (`80` for http, `443` for https,
`0` for unknown schemes).

```cdo
url.port("https://10.0.2.2:8443/health")     // -> 8443
url.port("http://example.com/")              // -> 80
```

### `url.path(url) -> string`

Path component including the leading `/`. Empty string if absent.

```cdo
url.path("https://10.0.2.2:8443/health")     // -> "/health"
```

### Behaviour

- Userinfo (`user:pass@`) and query (`?key=val`) / fragment (`#frag`)
  components are not separately exposed. They're consumed as part of
  the host / path respectively.
- IPv6 literals (`[::1]:8443`) are not specially handled. Stick to
  IPv4 or hostnames.
