# https — HTTPS GET via Mbed TLS

## `https.get(url, [hostname]) -> string|null`

Fetch the body at `url` (must be `https://...`). The URL form of
[`tls.httpsGet`](tls.md) — it parses `host` / `port` / `path` out of
the URL for you, then runs the same TLS pipeline. The optional
`hostname` overrides the SNI / expected cert CN (defaults to
`"canboot-test"`). Returns the response body, or `null` on TLS
handshake failure / HTTP error / timeout.

```cdo
https.get("https://10.0.2.2:8443/health")   // -> "canboot-secure"
```

CA pinning: the canboot test CA at
`tests/sidecars/tls/canboot-test.pem` is embedded at build time via
`tests/selftest/ca.c`. Regenerate / add your own CA with
`tests/selftest/embed-test-ca.sh`. See [`tls`](tls.md) for the full
behaviour notes (cipher suite, entropy source, session resumption).

## See also

- [`tls`](tls.md) — same pipeline, symmetric naming with `net.*`
- [`http`](http.md) — cleartext GET
- [`url`](url.md) — URL parser
- [`../networking.md`](../networking.md) — TLS pipeline
