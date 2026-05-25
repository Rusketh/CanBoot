# https — HTTPS GET via Mbed TLS

## `https.get(url) -> string|null`

Fetch the body at `url` (must be `https://...`). Same TLS pipeline as
[`tls.httpsGet`](tls.md); differs only in name. Returns the response
body, or `null` on TLS handshake failure / HTTP error / timeout.

```cdo
https.get("https://10.0.2.2:8443/health")   // -> "canboot-secure"
```

CA pinning: the canboot test CA at
`tests/selftest/ca/canboot-test.pem` is embedded at build time. Add
your own CA via `tests/selftest/embed-test-ca.sh`. See [`tls`](tls.md)
for the full behaviour notes (cipher suite, entropy source, session
resumption).

## See also

- [`tls`](tls.md) — same pipeline, symmetric naming with `net.*`
- [`http`](http.md) — cleartext GET
- [`url`](url.md) — URL parser
- [`../networking.md`](../networking.md) — TLS pipeline
