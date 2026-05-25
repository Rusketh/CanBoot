# `http` — HTTP GET (cleartext)

URL-aware GET helpers over lwIP. For HTTPS use [`https.*`](https.md);
for UDP echo see [`net.udpEcho`](net.md).

## API

### `http.get(url) -> string|null`

Fetch the body at `url` (must be `http://...`). Returns the response
body, or `null` on connection failure / non-2xx response / timeout.

```cdo
http.get("http://10.0.2.2:8080/hello")   // -> "canboot-hello"
```

### `http.status(url) -> number`

Issue the GET and return just the HTTP status code (e.g. `200`,
`404`). Returns `0` on connection failure / timeout.

```cdo
IF (http.status("http://10.0.2.2:8080/health") == 200) {
    print("service up");
}
```

## Behaviour

- URL parsing is the naive `host:port/path` form. Stick to explicit
  ports for anything non-standard.
- Response body cap is ~4 KiB for `http.status` (it reads the body
  to find the status line but discards it) and larger for `http.get`.
- Same lwIP path as [`net.httpGet`](net.md); the network stack is
  pumped cooperatively so audio + input keep working during the call.
