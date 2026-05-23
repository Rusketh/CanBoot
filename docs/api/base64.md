# `base64` — RFC 4648 base64

## API

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

## Behaviour

- Standard alphabet only (`A-Z a-z 0-9 + /`). The URL-safe variant
  (`-` / `_`) isn't separately exposed; pre-transform the input if
  you need it.
- Padding-required, not padding-optional. Inputs without `=` padding
  are rejected.
