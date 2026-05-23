# `https` — HTTPS GET via TLS 1.2

## API

### `https.get(url) -> string|null`

Fetch the body at `url` (must be `https://...`). Same TLS pipeline as
[`tls.httpsGet`](tls.md); differs only in name. Returns the response
body, or `null` on TLS handshake failure / HTTP error / timeout.

```cdo
https.get("https://10.0.2.2:8443/health")   // -> "canboot-secure"
```

## See also

- [tls.md](tls.md) — same call, different name
- [networking.md](../networking.md) — TLS pipeline details
- CA pinning: the canboot test CA at `tests/sidecars/tls/canboot-test.pem`
  is embedded at build time. Add your own CA via `scripts/embed-test-ca.sh`.
