# net — UDP + HTTP over lwIP

Convenience socket-style helpers wrapping lwIP. All calls block until
they get a response (or the timeout fires); the network stack is
pumped cooperatively from `time.ms` / `time.sleep` / `input.waitKey`,
so audio + input keep working during network calls.

## `net.lookup(host) -> array`

Resolve `host` to its IP addresses via lwIP's DNS. Returns an array of
dotted-quad strings (empty array if resolution fails or times out).

```cdo
VAR ips = net.lookup("example.com");
print(ips);
```

## `net.udpEcho(host, port, payload) -> string|null`

Send a UDP datagram to `host:port` and wait for a reply. `host` is
either a dotted-quad IP literal (`"10.0.2.2"`) or a hostname
(DNS-resolved via lwIP). Returns the reply bytes, or `null` on
timeout.

```cdo
VAR reply = net.udpEcho("10.0.2.2", 7777, "ping");
```

## `net.httpGet(url) -> string|null`

Issue an HTTP GET to the given URL. Returns the response body. `null`
on connection failure, non-2xx response, or timeout.

```cdo
VAR body = net.httpGet("http://10.0.2.2:8080/hello");
print(body);   // -> "canboot-hello"
```

URL parsing is naive — `host:port/path` form is the safe shape. For
the URL-aware variant, use [`http.get`](http.md).

## Behaviour

- DHCP is run by the kernel at boot; by the time `/init.cdo` gets to
  `net.*`, the netif is up at whatever lease SLIRP / the host DHCP
  server handed out.
- Default timeouts: ~5 seconds for HTTP, ~3 seconds for UDP echo.
  Both block on lwIP's `select`-equivalent path.
- Single virtio-net device only. The HAL surface allows multiple, but
  cando exposes only the primary.

## See also

- [`http`](http.md) — URL-aware cleartext HTTP GET
- [`https`](https.md) / [`tls`](tls.md) — TLS-protected fetch
- [`url`](url.md) — URL parser
- [`../networking.md`](../networking.md) — lwIP + virtio-net + Mbed TLS pipeline
