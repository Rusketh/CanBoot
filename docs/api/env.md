# `env` — boot environment introspection

What the loader handed us: boot source, framebuffer descriptor,
memory-map summary, platform tables pointer. All read from
`boot_info` once at kmain time, so the values are fixed for the
lifetime of boot.

## API

### `env.source() -> string`

How the kernel was booted. `"bios"`, `"uefi"`, or `"direct"`
(aarch64 `-kernel` path).

### `env.fbFormat() -> string`

Framebuffer encoding. `"rgb"` for standard packed-pixel formats,
`"none"` when no framebuffer is available.

### `env.fbWidth() -> number`

Framebuffer width in pixels. `0` when there's no framebuffer.

### `env.fbHeight() -> number`

Framebuffer height in pixels.

### `env.fbBpp() -> number`

Framebuffer bits per pixel. Typically `32`.

### `env.fbAddr() -> number`

Physical address of the framebuffer's linear backing buffer. `0`
when there's no framebuffer. Useful for sanity-checking which
scanout the painter is writing to.

### `env.mmapCount() -> number`

Number of entries in the boot memory map.

### `env.usableBytes() -> number`

Sum of `usable` regions in the memory map — i.e. how much RAM is
available. Lets you report "we have N MB" without parsing the full
map.

### `env.platformTables() -> number`

Pointer to the platform configuration tables: the ACPI RSDP on
x86_64, or the flattened device tree (FDT) base on aarch64. `0` if
the loader didn't find one.

## Example

```cdo
print("env.source =", env.source());
print("env.fbFormat =", env.fbFormat());
print("env.fbWidth =", env.fbWidth());
print("env.fbHeight =", env.fbHeight());
print("env.fbBpp =", env.fbBpp());
print("env.mmapCount =", env.mmapCount());
print("env.usableBytes =", env.usableBytes());
```

## Behaviour

- All values are snapshots from `boot_info`; they never change after
  boot.
- `fbAddr` / `platformTables` are raw physical addresses returned as
  numbers — on a 64-bit address, the value fits in a JS double up to
  2^53 which covers any realistic physical address.
