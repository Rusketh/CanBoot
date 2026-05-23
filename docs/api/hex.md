# `hex` — hex encoding

## API

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
