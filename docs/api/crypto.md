# crypto — hashing, HMAC, KDFs, ciphers

Hashes, HMAC, KDFs, symmetric ciphers, entropy + a timing-safe
compare, all backed by Mbed TLS. The surface follows CanDo's upstream
`crypto.*` module so scripts written against CanDo on a host port run
unchanged.

Every hash/HMAC/KDF function takes an optional trailing **encoding**
argument: `"hex"` (default) or `"bytes"` (raw binary). Binary input is
accepted everywhere — pass `fs.read(...)` output directly without a
hex round-trip.

## Convenience hashers

`crypto.md5`, `crypto.sha1`, `crypto.sha224`, `crypto.sha256`,
`crypto.sha384`, `crypto.sha512` — each `(data, enc?) -> string`.

```cdo
crypto.sha256("")
// -> "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
crypto.sha256("hello", "bytes")   // -> 32 raw bytes
crypto.md5("canboot")             // -> 32-char hex
```

## `crypto.hash(algo, data, enc?) -> string`

Generic hash. `algo` is one of `"md5"`, `"sha1"`, `"sha224"`,
`"sha256"`, `"sha384"`, `"sha512"`. Throws `EINVAL` for an unknown
algorithm.

```cdo
crypto.hash("sha384", "data");
```

## `crypto.hmac(algo, key, data, enc?) -> string`

Keyed HMAC over any supported `algo`.

```cdo
crypto.hmac("sha256", "key", "message");
```

## `crypto.timingSafeEqual(a, b) -> bool`

Constant-time byte comparison. Use this — never `==` — when checking a
MAC / token against an expected value, so the comparison time doesn't
leak how many bytes matched.

```cdo
IF (crypto.timingSafeEqual(got_mac, want_mac)) { ... }
```

## `crypto.randomBytes(n) -> string`, `crypto.randomInt(min, max) -> number`, `crypto.randomUUID() -> string`

Same DRBG source as the [`random`](random.md) lib, exposed under
CanDo's `crypto.*` names.

## KDFs

### `crypto.pbkdf2(password, salt, iterations, keylen, algo?, enc?) -> string`

PBKDF2-HMAC. `iterations` 1..10,000,000; `keylen` 1..256 bytes; `algo`
defaults to `"sha256"`, encoding defaults to hex.

```cdo
crypto.pbkdf2("pw", "salt", 100000, 32);   // 64-char hex key
```

### `crypto.hkdf(secret, salt, info, length, algo?, enc?) -> string`

HKDF (RFC 5869). `length` 1..256 bytes.

```cdo
crypto.hkdf(ikm, salt, "context", 32);
```

## Symmetric ciphers

### `crypto.encrypt(algo, key, iv, data, aad?, enc?) -> string`
### `crypto.decrypt(algo, key, iv, data, aad?, enc?) -> string`

Non-AEAD block / stream ciphers:

- `algo`: `"aes-128-cbc"`, `"aes-192-cbc"`, `"aes-256-cbc"`,
  `"aes-128-ctr"`, `"aes-192-ctr"`, `"aes-256-ctr"`
- `key`: 16 / 24 / 32 bytes for AES-128 / 192 / 256
- `iv`: 16 raw bytes
- `data`: plaintext (encrypt) / ciphertext (decrypt)
- `aad`: reserved — must be empty for these non-AEAD modes (passing a
  non-empty value throws, to surface a mistaken AEAD assumption)
- `enc`: `"hex"` (default) or `"bytes"`

CBC uses PKCS#7 padding; CTR is unpadded. Output capped at 8192 bytes.

```cdo
VAR key = crypto.randomBytes(32);
VAR iv  = crypto.randomBytes(16);
VAR ct  = crypto.encrypt("aes-256-cbc", key, iv, "secret", "", "bytes");
VAR pt  = crypto.decrypt("aes-256-cbc", key, iv, ct,       "", "bytes");
```

> AEAD modes (GCM, ChaCha20-Poly1305) aren't wired up yet — they return
> a `{ ciphertext, tag }` object that's getting focused review in a
> follow-up.

## Legacy aliases

Kept for older scripts + the smoke tests:

| Alias | Equivalent |
|-------|-----------|
| `crypto.sha256Hex(data)`        | `crypto.sha256(data)` |
| `crypto.sha512Hex(data)`        | `crypto.sha512(data)` |
| `crypto.sha256Raw(data)`        | `crypto.sha256(data, "bytes")` — raw 32 bytes |
| `crypto.sha512Raw(data)`        | `crypto.sha512(data, "bytes")` — raw 64 bytes |
| `crypto.hmacSha256(key, data)`  | `crypto.hmac("sha256", key, data, "bytes")` |
| `crypto.hmacSha256Hex(key, data)` | `crypto.hmac("sha256", key, data)` |

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
- `"base64"` / `"base64url"` encodings for the `enc?` argument land in a
  follow-up; today it's `"hex"` or `"bytes"` only. For base64 output,
  pipe through [`base64.encode`](base64.md).

## See also

- [`random`](random.md) — entropy + RNG helpers (same DRBG source)
- [`hex`](hex.md) / [`base64`](base64.md) — encode binary digests for display
- [`../networking.md`](../networking.md) — the Mbed TLS port these routines share
