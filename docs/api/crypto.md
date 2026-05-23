# crypto — hashing, entropy, encoding

Hashes + HMAC backed by Mbed TLS, an entropy source shared with the TLS
layer, and the two encodings you'll want to pair them with.

- [`crypto`](#crypto) — SHA-256/512 + HMAC
- [`random`](#random) — entropy + RNG helpers
- [`hex`](#hex) — hex encoding
- [`base64`](#base64) — RFC 4648 base64

---

## crypto

Hash helpers backed by Mbed TLS. All functions return raw binary
strings (binary-safe — no NUL truncation). Pair with [`hex`](#hex) or
[`base64`](#base64) for printable encodings.

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

### Behaviour

- Functions accept binary input. Pass `fs.read(...)` output directly
  without round-tripping through hex.
- The Mbed TLS routines fall back to software implementations when no
  hardware accelerator is detected. Performance is fine for hashing
  init payloads / verifying small files; bulk hashing of large media
  is slower than native code.

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
