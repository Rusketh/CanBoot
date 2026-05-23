# `env` — boot environment introspection

What the loader handed us: framebuffer format, memory map summary,
boot source.

## API

### `env.source() -> string`

How the kernel was booted. `"bios"`, `"uefi"`, or `"direct"` (aarch64
`-kernel` path).

### `env.fbFormat() -> string`

Framebuffer encoding. `"rgb"` for standard packed-pixel RGBA-like
formats, `"none"` when no framebuffer is available.

### `env.fbWidth() -> number`

Framebuffer width in pixels. `0` when there's no framebuffer.

### `env.fbHeight() -> number`

Framebuffer height in pixels.

### `env.mmapCount() -> number`

Number of entries in the boot memory map.

### `env.usableBytes() -> number`

Sum of `usable` regions in the memory map. Useful for reporting
"we have N MB of RAM" without parsing the full map.

```cdo
print("env.source =", env.source());
print("env.fbFormat =", env.fbFormat());
print("env.fbWidth =", env.fbWidth());
print("env.fbHeight =", env.fbHeight());
print("env.usableBytes =", env.usableBytes());
```

## Behaviour

- Values are read from `boot_info` once at kmain time. Subsequent
  calls return the same numbers.
