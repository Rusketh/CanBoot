# http — cleartext HTTP GET

URL-aware HTTP GET helper. Same wire path as [`net.httpGet`](net.md)
but reads more naturally next to [`https.get`](https.md) since both
take a full URL.

## `http.get(url) -> string|null`

Fetch the body at `url` (must be `http://...`). Returns the response
body, or `null` on connection failure / non-2xx response / timeout.

```cdo
http.get("http://10.0.2.2:8080/hello")   // -> "canboot-hello"
```

## `http.status(url) -> number`

Issue the GET and return just the HTTP status code (e.g. `200`,
`404`). Returns `0` on connection failure / timeout.

```cdo
IF (http.status("http://10.0.2.2:8080/health") == 200) {
    print("service up");
}
```

For HTTPS, use [`https.get`](https.md). For UDP echo, see
[`net.udpEcho`](net.md).

## See also

- [`net`](net.md) — UDP echo + raw HTTP GET
- [`https`](https.md) — TLS-protected GET
- [`url`](url.md) — URL parser
