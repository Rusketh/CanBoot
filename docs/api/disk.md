# disk — raw block devices

Inspect attached storage at the LBA level. For filesystem-aware ops
use [`fs.*`](fs.md); for the convenience root-dir API use
[`file.*`](file.md).

## `disk.count() -> number`

Number of discovered block devices. Includes virtio-blk, AHCI, etc.
Boot disk is index `0`.

## `disk.name(i) -> string`

Driver-assigned name for disk `i`. Currently `"vblk0"`, `"vblk1"`,
... for virtio-blk and `"ahci0"`, ... for AHCI. Empty string for an
invalid index.

## `disk.blockSize(i) -> number`

Sector size in bytes. Typically 512.

## `disk.blocks(i) -> number`

Total block count. Multiply by `blockSize` for raw capacity in bytes.

```cdo
print("disk count:", disk.count());
FOR (VAR i = 0; i < disk.count(); i = i + 1) {
    print("  ", disk.name(i), disk.blocks(i), "blocks of", disk.blockSize(i));
}
```

## `disk.writable(i) -> bool`

`true` if the device accepts writes. CD-ROM / read-only media report
`false`.

## `disk.read(i, lba, count) -> string|null`

Read `count` blocks starting at `lba` from disk `i`. Returns the raw
bytes as a binary string (length = `count * blockSize`), or `null` on
error / out-of-range / if the request exceeds the 64 KiB transfer cap.

```cdo
VAR mbr = disk.read(0, 0, 1);          // first sector
print("MBR sig:", hex.encode(mbr));
```

## `disk.write(i, lba, data) -> bool`

Write `data` to disk `i` starting at `lba`. `data`'s length **must be
a whole multiple of the block size** — partial-block writes are
rejected. Capped at 64 KiB per call. Returns `true` on success.

⚠ Raw writes bypass every filesystem. Writing to a live filesystem's
blocks corrupts it.

## Behaviour

- **Transfer cap**: a single `disk.read` / `disk.write` moves at most
  64 KiB. For larger ranges, loop.
- **Block alignment**: writes must be block-multiple-sized; a short
  buffer returns `false` rather than being padded.
- **Binary-safe reads**: `disk.read` returns a length-carrying string,
  so embedded NULs survive — pair with [`hex`](hex.md) for a
  printable dump.
- Up to 8 disks discoverable. Past 8 the HAL ignores additional
  devices.

## See also

- [`partition`](partition.md) — partition table read/write
- [`fs`](fs.md) — filesystem-aware file ops
- [`file`](file.md) — single-disk root-dir convenience
- [`../hal.md`](../hal.md) — HAL disk contract
