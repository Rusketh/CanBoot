# `tls` — HTTPS GET via Mbed TLS

Direct entry point for fetching HTTPS resources. Same underlying
TLS pipeline as [`https.*`](https.md); the difference is that `tls.*`
exists for symmetry with `net.*` and reads more naturally when the
script's intent is "raw protected fetch" rather than "I want this
URL's body."

## API

### `tls.httpsGet(url) -> string|null`

Fetch the body at the given `https://` URL using TLS 1.2. Uses the
canboot test CA embedded at build time (`tests/sidecars/tls/canboot-test.pem`)
to validate the chain. Returns the response body, or `null` on TLS
handshake failure, HTTP error, or timeout.

```cdo
VAR body = tls.httpsGet("https://10.0.2.2:8443/health");
```

The handshake auto-resumes via session tickets on repeat calls to the
same host:port — second connection lands ~30× faster than the first.

## Behaviour

- **CA pinning**: only certificates issued under the canboot test CA
  validate. Production CAs (Let's Encrypt etc.) aren't trusted in the
  current build — they'd need to be embedded the same way the test
  CA is (see `scripts/embed-test-ca.sh`).
- **Cipher suite**: TLS 1.2 ECDHE-RSA-AES256-GCM-SHA384 (handshake
  default).
- **Entropy source**: RDSEED / RDRAND on x86_64 when available
  (CPUID-gated to skip `qemu64` which lacks the bit), falling back to
  a TSC-jitter mixer.
- See [networking.md](../networking.md) for the full TLS pipeline.
