# `disk` — raw block device enumeration

Inspect attached storage at the LBA level. For filesystem-aware ops
use [`fs.*`](fs.md); for the convenience root-dir API use
[`file.*`](file.md).

## API

### `disk.count() -> number`

Number of discovered block devices. Includes virtio-blk, AHCI, etc.
Boot disk is index `0`.

### `disk.name(i) -> string`

Driver-assigned name for disk `i`. Currently `"vblk0"`, `"vblk1"`,
... for virtio-blk and `"ahci0"`, ... for AHCI. Empty string for an
invalid index.

### `disk.blockSize(i) -> number`

Sector size in bytes. Typically 512.

### `disk.blocks(i) -> number`

Total block count. Multiply by `blockSize` for raw capacity in bytes.

```cdo
print("disk count:", disk.count());
FOR (VAR i = 0; i < disk.count(); i = i + 1) {
    print("  ", disk.name(i), disk.blocks(i), "blocks of", disk.blockSize(i));
}
```

## Behaviour

- This API doesn't read or write the device. It only exposes
  enumeration. To read raw blocks, go through [`fs.*`](fs.md) /
  [`file.*`](file.md) or write at the C level.
- Up to 8 disks discoverable. Past 8 the HAL ignores additional
  devices.
