# random — entropy + RNG helpers

Pulls bytes from the platform RNG (RDSEED/RDRAND on x86_64,
`RNDR`/`RNDRRS` on ARMv8.5+) and falls back to a TSC-jitter mixer
when neither is available. Same source the TLS layer uses to seed
Mbed TLS's CTR-DRBG.

## `random.bytes(n) -> string`

`n` random bytes as a binary string.

```cdo
crypto.sha256Hex(random.bytes(32))
// -> 64 hex chars derived from 256 bits of entropy
```

## `random.hex(n) -> string`

`n` random bytes encoded as lowercase hex. Equivalent to
`hex.encode(random.bytes(n))` but skips the round-trip.

```cdo
random.hex(8)   // -> "42d5ebe57f29ac6b"
```

## `random.int(min, max) -> number`

Uniform random integer in `[min, max]` (both inclusive).

```cdo
random.int(1, 100);
```

## `random.float() -> number`

Uniform random float in `[0.0, 1.0)`, full double precision (built
from the high 53 bits of a 64-bit draw).

```cdo
random.float();   // e.g. 0.4137...
```

## `random.uuid() -> string`

RFC 4122 v4 UUID with hyphens, 36 characters.

```cdo
random.uuid()
// -> "d463c805-6e74-4fea-a1b4-5baef50969b9"
```

## See also

- [`crypto`](crypto.md) — hashing/HMAC; `crypto.randomBytes` etc. share this source
- [`hex`](hex.md) — encode raw random bytes for display
