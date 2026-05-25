# env — boot environment introspection

What the loader handed us: framebuffer format, memory map summary,
boot source.

## `env.source() -> string`

How the kernel was booted. `"bios"`, `"uefi"`, or `"direct"` (aarch64
`-kernel` path).

## `env.fbFormat() -> string`

Framebuffer encoding. `"rgb"` for standard packed-pixel RGBA-like
formats, `"none"` when no framebuffer is available.

## `env.fbWidth() -> number`

Framebuffer width in pixels. `0` when there's no framebuffer.

## `env.fbHeight() -> number`

Framebuffer height in pixels.

## `env.fbBpp() -> number`

Framebuffer bits per pixel. Typically `32`.

## `env.fbAddr() -> number`

Physical address of the framebuffer's linear backing buffer. `0` when
there's no framebuffer — handy for confirming which scanout the
painter writes to.

## `env.mmapCount() -> number`

Number of entries in the boot memory map.

## `env.usableBytes() -> number`

Sum of `usable` regions in the memory map. Useful for reporting
"we have N MB of RAM" without parsing the full map.

## `env.platformTables() -> number`

Pointer to the platform configuration tables — the ACPI RSDP on
x86_64, or the flattened device tree (FDT) base on aarch64. `0` if the
loader didn't find one.

```cdo
print("env.source =", env.source());
print("env.fbFormat =", env.fbFormat());
print("env.fbWidth =", env.fbWidth());
print("env.fbHeight =", env.fbHeight());
print("env.fbBpp =", env.fbBpp());
print("env.usableBytes =", env.usableBytes());
```

## Behaviour

- Values are read from `boot_info` once at kmain time. Subsequent
  calls return the same numbers.

## See also

- [`os`](os.md) — `os.arch` / `os.totalmem` / `os.platform` overlap with these
- [`../bootflow.md`](../bootflow.md) — how `boot_info` is populated
