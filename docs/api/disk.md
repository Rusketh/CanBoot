# `disk` — raw block device access

Enumerate attached storage and read/write raw LBA blocks. For
filesystem-aware ops use [`fs.*`](fs.md); for the convenience root-dir
API use [`file.*`](file.md).

## Enumerate

### `disk.count() -> number`

Number of discovered block devices. Includes virtio-blk, AHCI, etc.
Boot disk is index `0`.

### `disk.name(i) -> string`

Driver-assigned name for disk `i`. `"vblk0"`, `"vblk1"`, ... for
virtio-blk; `"ahci0"`, ... for AHCI. Empty string for an invalid
index.

### `disk.blockSize(i) -> number`

Sector size in bytes. Typically 512.

### `disk.blocks(i) -> number`

Total block count. Multiply by `blockSize` for raw capacity in bytes.

### `disk.writable(i) -> bool`

`true` if the device accepts writes. CD-ROM / read-only media report
`false`.

```cdo
FOR (VAR i = 0; i < disk.count(); i = i + 1) {
    print(disk.name(i), disk.blocks(i), "x", disk.blockSize(i),
          disk.writable(i) ? "rw" : "ro");
}
```

## Raw block I/O

### `disk.read(i, lba, count) -> string|null`

Read `count` blocks starting at `lba` from disk `i`. Returns the raw
bytes as a binary string (length = `count * blockSize`), or `null`
on error / out-of-range / if the request exceeds the 64 KiB transfer
cap.

```cdo
VAR mbr = disk.read(0, 0, 1);     // first sector
print("MBR sig:", hex.encode(mbr));
```

### `disk.write(i, lba, data) -> bool`

Write `data` to disk `i` starting at `lba`. `data`'s length **must be
a whole multiple of the block size** — partial-block writes are
rejected. Capped at 64 KiB per call. Returns `true` on success.

```cdo
VAR sector = ...;                 // exactly 512 bytes
disk.write(0, 100, sector);
```

⚠ Raw writes bypass every filesystem. Writing to a live filesystem's
blocks corrupts it.

## Behaviour

- **Transfer cap**: a single `disk.read` / `disk.write` moves at most
  64 KiB (`DISK_MAX_BYTES`). For larger ranges, loop.
- **Block alignment**: writes must be block-multiple-sized. `disk.write`
  returns `false` rather than padding a short buffer.
- **Binary-safe reads**: `disk.read` returns a length-carrying cando
  string, so embedded NULs survive — pair with [`hex`](hex.md) for a
  printable dump.
- Up to 8 disks discoverable; past 8 the HAL ignores additional
  devices.
