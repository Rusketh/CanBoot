# `crypto` — hashing + HMAC

Hash helpers backed by Mbed TLS. All functions return raw binary
strings (binary-safe — no NUL truncation). Pair with [`hex`](hex.md)
or [`base64`](base64.md) for printable encodings.

## API

### `crypto.sha256(data) -> string`

SHA-256 digest of `data`. 32-byte binary string.

```cdo
crypto.sha256("hello") // -> binary 32 bytes
```

### `crypto.sha256Hex(data) -> string`

SHA-256 digest as a 64-char lowercase hex string.

```cdo
crypto.sha256Hex("")
// -> "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
```

### `crypto.sha512(data) -> string`

SHA-512 digest. 64-byte binary string.

### `crypto.sha512Hex(data) -> string`

SHA-512 digest as a 128-char lowercase hex string.

### `crypto.hmacSha256(key, data) -> string`

HMAC-SHA256 of `data` keyed by `key`. 32-byte binary string.

### `crypto.hmacSha256Hex(key, data) -> string`

HMAC-SHA256 as 64-char hex.

```cdo
crypto.hmacSha256Hex("k", "m")
// -> "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
```

## Behaviour

- Functions accept binary input. Pass `fs.read(...)` output directly
  without round-tripping through hex.
- The Mbed TLS routines fall back to software implementations when no
  hardware accelerator is detected. Performance is fine for hashing
  init payloads / verifying small files; bulk hashing of large media
  is slower than native code.
