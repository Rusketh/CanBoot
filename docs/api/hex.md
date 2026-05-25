# hex — hex encode / decode

Lowercase hex codec, most often paired with [`crypto.*`](crypto.md)
for human-readable hash / HMAC output. Inputs and outputs are cando
strings, which carry an explicit length so binary payloads survive.

## `hex.encode(bytes) -> string`

Lowercase hex of the input bytes.

```cdo
hex.encode("canboot")   // -> "63616e626f6f74"
```

## `hex.decode(str) -> string`

Decode a hex string back to raw bytes. Accepts upper or lower case.
Returns `null` if the input length is odd or contains non-hex
characters.

```cdo
hex.decode("63616e626f6f74")   // -> "canboot"
```

## See also

- [`base64`](base64.md) — the other binary-to-text codec
- [`crypto`](crypto.md) — produces digests you'll often hex-encode
- [`random`](random.md) — `random.hex(n)` skips the encode round-trip
