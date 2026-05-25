# crypto — hashing, entropy, encoding

Hashes + HMAC backed by Mbed TLS, an entropy source shared with the TLS
layer, and the two encodings you'll want to pair them with.

- [`crypto`](#crypto) — SHA-256/512 + HMAC
- [`random`](#random) — entropy + RNG helpers
- [`hex`](#hex) — hex encoding
- [`base64`](#base64) — RFC 4648 base64

---

## crypto

Hashes, HMAC, KDFs, symmetric ciphers, entropy + a timing-safe
compare, all backed by Mbed TLS. The surface follows CanDo's upstream
`crypto.*` module so scripts written against CanDo on a host port run
unchanged.

Every hash/HMAC/KDF function takes an optional trailing **encoding**
argument: `"hex"` (default) or `"bytes"` (raw binary). Binary input is
accepted everywhere — pass `fs.read(...)` output directly without a
hex round-trip.

### Convenience hashers

`crypto.md5`, `crypto.sha1`, `crypto.sha224`, `crypto.sha256`,
`crypto.sha384`, `crypto.sha512` — each `(data, enc?) -> string`.

```cdo
crypto.sha256("")
// -> "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
crypto.sha256("hello", "bytes")   // -> 32 raw bytes
crypto.md5("canboot")             // -> 32-char hex
```

### `crypto.hash(algo, data, enc?) -> string`

Generic hash. `algo` is one of `"md5"`, `"sha1"`, `"sha224"`,
`"sha256"`, `"sha384"`, `"sha512"`. Throws `EINVAL` for an unknown
algorithm.

```cdo
crypto.hash("sha384", "data");
```

### `crypto.hmac(algo, key, data, enc?) -> string`

Keyed HMAC over any supported `algo`.

```cdo
crypto.hmac("sha256", "key", "message");
```

### `crypto.timingSafeEqual(a, b) -> bool`

Constant-time byte comparison. Use this — never `==` — when checking a
MAC / token against an expected value, so the comparison time doesn't
leak how many bytes matched.

```cdo
IF (crypto.timingSafeEqual(got_mac, want_mac)) { ... }
```

### `crypto.randomBytes(n) -> string`, `crypto.randomInt(min, max) -> number`, `crypto.randomUUID() -> string`

Same DRBG source as the [`random`](#random) lib, exposed under CanDo's
`crypto.*` names.

### KDFs

#### `crypto.pbkdf2(password, salt, iterations, keylen, algo?, enc?) -> string`

PBKDF2-HMAC. `iterations` 1..10,000,000; `keylen` 1..256 bytes; `algo`
defaults to `"sha256"`, encoding defaults to hex.

```cdo
crypto.pbkdf2("pw", "salt", 100000, 32);   // 64-char hex key
```

#### `crypto.hkdf(secret, salt, info, length, algo?, enc?) -> string`

HKDF (RFC 5869). `length` 1..256 bytes.

```cdo
crypto.hkdf(ikm, salt, "context", 32);
```

### Symmetric ciphers

#### `crypto.encrypt(algo, key, iv, data, aad?, enc?) -> string`
#### `crypto.decrypt(algo, key, iv, data, aad?, enc?) -> string`

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

### Legacy aliases

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

### Behaviour

- Functions accept binary input. Pass `fs.read(...)` output directly
  without round-tripping through hex.
- The Mbed TLS routines fall back to software implementations when no
  hardware accelerator is detected. Performance is fine for hashing
  init payloads / verifying small files; bulk hashing of large media
  is slower than native code.
- `"base64"` / `"base64url"` encodings for the `enc?` argument land in a
  follow-up; today it's `"hex"` or `"bytes"` only. For base64 output,
  pipe through [`base64.encode`](#base64).

---

## random

Pulls bytes from the platform RNG (RDSEED/RDRAND on x86_64,
`RNDR`/`RNDRRS` on ARMv8.5+) and falls back to a TSC-jitter mixer
when neither is available. Same source the TLS layer uses to seed
Mbed TLS's CTR-DRBG.

### `random.bytes(n) -> string`

`n` random bytes as a binary string.

```cdo
crypto.sha256Hex(random.bytes(32))
// -> 64 hex chars derived from 256 bits of entropy
```

### `random.hex(n) -> string`

`n` random bytes encoded as lowercase hex. Equivalent to
`hex.encode(random.bytes(n))` but skips the round-trip.

```cdo
random.hex(8)   // -> "42d5ebe57f29ac6b"
```

### `random.int(min, max) -> number`

Uniform random integer in `[min, max]` (both inclusive).

```cdo
random.int(1, 100);
```

### `random.float() -> number`

Uniform random float in `[0.0, 1.0)`, full double precision (built
from the high 53 bits of a 64-bit draw).

```cdo
random.float();   // e.g. 0.4137...
```

### `random.uuid() -> string`

RFC 4122 v4 UUID with hyphens, 36 characters.

```cdo
random.uuid()
// -> "d463c805-6e74-4fea-a1b4-5baef50969b9"
```

---

## hex

### `hex.encode(bytes) -> string`

Lowercase hex of the input bytes.

```cdo
hex.encode("canboot")   // -> "63616e626f6f74"
```

### `hex.decode(str) -> string`

Decode a hex string back to raw bytes. Accepts upper or lower case.
Returns `null` if the input length is odd or contains non-hex
characters.

```cdo
hex.decode("63616e626f6f74")   // -> "canboot"
```

---

## base64

### `base64.encode(bytes) -> string`

Encode the input bytes. Pads with `=`.

```cdo
base64.encode("canboot")   // -> "Y2FuYm9vdA=="
```

### `base64.decode(str) -> string`

Decode. Returns `null` on invalid input (bad characters, wrong
padding).

```cdo
base64.decode("Y2FuYm9vdA==")   // -> "canboot"
```

### Behaviour

- Standard alphabet only (`A-Z a-z 0-9 + /`). The URL-safe variant
  (`-` / `_`) isn't separately exposed; pre-transform the input if
  you need it.
- Padding-required, not padding-optional. Inputs without `=` padding
  are rejected.
