# `http` — HTTP GET (cleartext)

URL-aware GET helper. Same wire path as [`net.httpGet`](net.md) but
reads more naturally next to `https.get` since both take a full URL.

## API

### `http.get(url) -> string|null`

Fetch the body at `url` (must be `http://...`). Returns the
response body, or `null` on connection failure / non-2xx response /
timeout.

```cdo
http.get("http://10.0.2.2:8080/hello")   // -> "canboot-hello"
```

For HTTPS, use [`https.get`](https.md) — the same shape but uses
Mbed TLS.

For UDP echo, see [`net.udpEcho`](net.md).
