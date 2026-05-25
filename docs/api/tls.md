# tls — TLS-protected fetch

The raw socket-style entry point for HTTPS. It shares the same
underlying TLS pipeline as [`https.get`](https.md) but takes its target
as **separate `host` / `port` / `path` arguments** (the same shape as
[`net.httpGet`](net.md)), rather than a single URL. Use it when the
script already has the pieces split; use `https.get` when you have a
full `https://` URL.

## `tls.httpsGet(host, port, path, [hostname]) -> string|null`

Fetch `path` from `host:port` over TLS 1.2. Arguments:

- `host` — dotted-quad IPv4 literal (e.g. `"10.0.2.2"`). Not
  DNS-resolved on this path.
- `port` — TCP port. Defaults to `443`.
- `path` — request path. Defaults to `"/"`.
- `hostname` — optional SNI / expected cert CN. Defaults to
  `"canboot-test"` so the bundled test CA verifies.

Uses the canboot test CA embedded at build time to validate the chain.
Returns the response body on a chain-validated `200`, or `null` on
connect / handshake failure, non-200 status, or timeout.

```cdo
VAR body = tls.httpsGet("10.0.2.2", 8443, "/health");
```

The handshake auto-resumes via session tickets on repeat calls to the
same host:port — second connection lands ~30× faster than the first.

## Behaviour

- **CA pinning**: only certificates issued under the canboot test CA
  (`tests/sidecars/tls/canboot-test.pem`, embedded via
  `tests/selftest/ca.c`) validate. Production CAs (Let's Encrypt etc.)
  aren't trusted in the current build — they'd need to be embedded the
  same way the test CA is.
- **Cipher suite**: TLS 1.2 ECDHE-RSA-AES256-GCM-SHA384 (handshake
  default).
- **Entropy source**: RDSEED / RDRAND on x86_64 when available
  (CPUID-gated to skip `qemu64` which lacks the bit), falling back to
  a TSC-jitter mixer.
- See [`../networking.md`](../networking.md) for the full TLS
  pipeline.

## See also

- [`https`](https.md) — same pipeline, URL-body framing
- [`http`](http.md) — cleartext GET
- [`net`](net.md) — UDP + raw HTTP
