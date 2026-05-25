# url — URL parser

Lightweight URL parser. Splits a `scheme://host[:port]/path` string
into named fields.

## `url.scheme(url) -> string`

Lowercase scheme. `"http"`, `"https"`, etc. Empty string if missing.

```cdo
url.scheme("https://10.0.2.2:8443/health")   // -> "https"
```

## `url.host(url) -> string`

Host part. IP literal or hostname. Empty string if missing.

```cdo
url.host("https://10.0.2.2:8443/health")     // -> "10.0.2.2"
```

## `url.port(url) -> number`

Port number, or the scheme default (`80` for http, `443` for https,
`0` for unknown schemes).

```cdo
url.port("https://10.0.2.2:8443/health")     // -> 8443
url.port("http://example.com/")              // -> 80
```

## `url.path(url) -> string`

Path component including the leading `/`. Empty string if absent.

```cdo
url.path("https://10.0.2.2:8443/health")     // -> "/health"
```

## Behaviour

- Userinfo (`user:pass@`) and query (`?key=val`) / fragment (`#frag`)
  components are not separately exposed. They're consumed as part of
  the host / path respectively.
- IPv6 literals (`[::1]:8443`) are not specially handled. Stick to
  IPv4 or hostnames.

## See also

- [`http`](http.md) / [`https`](https.md) — fetch a parsed URL
- [`net`](net.md) — UDP + raw HTTP
