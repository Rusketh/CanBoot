# storage — disks, partitions, filesystems

Block devices, partition tables, filesystem-aware file ops, single-disk
root-directory convenience API, and PCI bus enumeration.

- [`disk`](#disk) — raw block device enumeration
- [`partition`](#partition) — GPT + MBR partition table read
- [`fs`](#fs) — filesystem-aware read/write/delete/mkfs
- [`file`](#file) — single-disk root-directory convenience
- [`pci`](#pci) — PCI(e) bus walk

---

## disk

Inspect attached storage at the LBA level. For filesystem-aware ops
use [`fs.*`](#fs); for the convenience root-dir API use
[`file.*`](#file).

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

### Behaviour

- This API doesn't read or write the device. It only exposes
  enumeration. To read raw blocks, go through [`fs.*`](#fs) /
  [`file.*`](#file) or write at the C level.
- Up to 8 disks discoverable. Past 8 the HAL ignores additional
  devices.

---

## partition

Read-only enumeration. Partition creation / resize / delete happens at
the C level (`fs/partition.c`); the cando surface exposes only
inspection.

### `partition.scheme(disk) -> string`

Partition table format on `disk`. One of:

- `"gpt"` — GPT signature + valid header
- `"mbr"` — MBR signature without GPT
- `"none"` — neither (whole-disk filesystem)

### `partition.count(disk) -> number`

Number of partitions in the table. `0` for `"none"` scheme.

### `partition.entry(disk, idx) -> string`

Human-readable summary of partition `idx`:

```
"start=2048 size=131072 type=83 name=ROOT"
```

Fields:

- `start` — first LBA (inclusive)
- `size`  — count in LBAs
- `type`  — MBR type byte (0x83 for Linux) or GPT type UUID's first byte
- `name`  — GPT partition name (empty for MBR)

The exact format is intentionally a single string rather than a
structured object so scripts can `print()` it cleanly and parse with
regex if needed.

### Behaviour

- GPT primary header at LBA 1 is the only one consulted. If the
  primary is corrupted but the backup at the last LBA is valid, this
  API reports `"none"`. The C-level `canboot_part_list` does try the
  backup; the cando surface doesn't expose that yet.
- Up to 128 partitions per disk are enumerated.
- For "what filesystem is on this partition", see [`fs.detect`](#fs).

---

## fs

`fs.*` targets a specific `(disk, partition)` tuple and dispatches by
filesystem type. Read/write/delete works against FAT32, NTFS (via
libntfs-3g), ext2/3/4 (via lwext4) and ISO9660. Format works for
FAT32, NTFS, and ext2/3/4.

Where [`file.*`](#file) ignores partitions and walks the root of the
first writable disk, `fs.*` is the one to use for multi-partition
disks and non-FAT32 filesystems.

### Quick recap

```cdo
print("disk 1 part 0 fs =", fs.detect(1, 0));   // -> "ntfs" / "ext4" / "fat32" / ...
print("label =", fs.label(1, 0));
print(fs.read(1, 0, "/probe.txt"));
fs.write(1, 0, "/hello.txt", "world");
fs.delete(1, 0, "/old.txt");

fs.mkfs(1, 0, "ntfs", "MYDISK");                // reformat
```

### Detect + introspect

#### `fs.detect(disk, part) -> string`

Returns the filesystem name found at `(disk, part)`. One of:

- `"fat32"` — FAT32 BPB found at the partition's first sector
- `"fat16"` — FAT16 fallback
- `"ntfs"` — `"NTFS"` signature at offset 3 of the boot sector
- `"ext4"` — ext superblock magic `0xEF53` at byte 1080 of the partition
- `"iso9660"` — `"CD001"` at LBA 16
- `"unknown"` — nothing matched

The detector reads at most two sectors; it's safe to call repeatedly.

When `part` is `0` and the disk has no partition table, `fs.*`
synthesises a whole-disk "partition 0" covering the entire disk.

#### `fs.label(disk, part) -> string`

Volume label. Works for FAT32 (BPB ext-FS label), NTFS (the unicode
volume name stripped to ASCII), and ext4 (`s_volume_name` from the
superblock). Returns `""` for filesystems without a label or when no
label was set.

#### `fs.totalBytes(disk, part) -> number`

Size of the partition in bytes. Computed from `(part.size_lba * block_size)`,
not from any filesystem free-space field. Useful for "how big is this
partition" but not for "how much free space is there".

#### `fs.usedBytes(disk, part) -> number`

Currently always `0` — full used-bytes accounting needs per-FS
metadata walks (FAT chain scan, NTFS $Bitmap, ext bitmap aggregation)
that aren't implemented yet. Returns `0` as "unknown" rather than
fabricating a number.

### Read / write / delete

#### `fs.read(disk, part, path) -> string|null`

Read a file by path. Binary-safe — the returned cando string carries
the file length as a separate field so embedded NULs survive.

Filesystem-specific behaviour:

| FS | Path convention | Notes |
|----|-----------------|-------|
| FAT32 | Leading `/` is stripped; 8.3 names only; root dir only | In-tree driver |
| NTFS  | Full path (`/dir/file`); leading `/` required | Goes via libntfs-3g; falls back to in-tree read-only driver if mount fails |
| ext4  | Full path (`/dir/file`); leading `/` required | Goes via lwext4 |
| ISO9660 | Bare name; no subdirs | In-tree driver |

Returns `null` for missing files, FS detection failures, etc.

Total buffer is ~64 KiB. Larger files are truncated.

#### `fs.write(disk, part, path, data) -> bool`

Write `data` (string) to `path`, replacing any existing file. Returns
`true` on success.

Upsert semantics on NTFS — tries to write to an existing inode first,
falls back to creating a fresh inode with the payload as initial
`$DATA` content.

FAT32 write supports only the root directory. NTFS and ext4 support
nested paths.

#### `fs.delete(disk, part, path) -> bool`

Remove the file at `path`. Returns `true` if a file was removed or
`false` if it didn't exist / the FS isn't writable.

Doesn't recurse into directories.

### Format

#### `fs.mkfs(disk, part, type, label) -> bool`

Wipe the partition and lay down a fresh filesystem of `type`:

- `"fat32"` — in-tree formatter
- `"ntfs"` — vendored mkntfs (libntfs-3g/ntfsprogs)
- `"ext2"`, `"ext3"`, `"ext4"` — vendored lwext4
- anything else — returns `false`

The `label` string becomes the volume name in the new filesystem.

Returns `true` if mkfs succeeded.

⚠ **mkfs destroys data on the target partition.** No prompt, no
double-check.

### Behaviour notes

#### Multiple mounts

Calling `fs.read` and then `fs.write` on the same NTFS volume in
sequence works correctly — each call mounts, operates, unmounts.
There's no persistent mount handle exposed at the cando level, so two
parallel writes from cando aren't possible (and wouldn't make sense
single-threaded).

#### Path separators

Forward slash `/` everywhere — even on FAT32 where the on-disk format
uses backslash semantics. The library strips the leading `/` before
calling FAT32's bare-name lookup.

#### Whole-disk partitions

If a disk has no GPT/MBR partition table, `fs.detect/read/write/delete`
all treat `(disk, 0)` as the whole disk. The smoke tests use this for
the FAT32 boot disk and the NTFS / ext4 test images.

---

## file

A convenience wrapper around the FAT32 driver. Operates on the first
writable disk's root directory. For multi-partition access or
non-FAT32 filesystems use [`fs.*`](#fs) instead.

### `file.exists(name) -> bool`

`true` iff a file with that name exists in the root directory.

### `file.size(name) -> number`

Size in bytes, or `0` if missing.

### `file.read(name) -> string|null`

Read the file's bytes. Binary-safe (strings carry an explicit length).
Returns `null` if missing. Truncated at ~64 KiB.

### `file.write(name, data) -> bool`

Write `data` to the file, creating it or replacing existing content.
Returns `true` on success.

### `file.list() -> string`

Newline-separated names of every entry in the root directory. Empty
string if the disk has no files.

```cdo
VAR names = file.list();
print(names);   // INIT.CDO\nPROBE.PNG\n...
```

### Behaviour

- Names follow FAT32 8.3 conventions on disk; the API accepts mixed
  case and either with or without a leading `/`.
- Only the FAT32 driver is exposed. ISO9660 + NTFS + ext4 are reachable
  via [`fs.*`](#fs) only.

---

## pci

### `pci.count() -> number`

Number of PCI functions discovered. Includes all buses / devices /
functions populated at HAL init.

### `pci.list() -> string`

Newline-separated lines, one per function, in the format:

```
BB:DD.F VEND:DEV class=CC:SC:PI
```

- `BB:DD.F` — bus / device / function in hex
- `VEND:DEV` — vendor + device ID
- `class=CC:SC:PI` — class / subclass / programming interface

```
00:00.0 1b36:0008 class=06:00:00
00:01.0 1af4:1001 class=01:00:00
00:02.0 1af4:1001 class=01:00:00
00:03.0 1af4:1052 class=09:00:00
00:04.0 1af4:1050 class=03:80:00
00:05.0 1af4:1000 class=02:00:00
```

### Behaviour

- Up to 256 functions enumerated. The HAL scans buses 0..127 / devs
  0..31 / funcs 0..7.
- No BAR / capability inspection at the cando level — that lives in
  the HAL for drivers. The cando surface is for "what's on the bus";
  drivers use the C API directly.

## See also

- [`../filesystems.md`](../filesystems.md) — how the FAT32 / NTFS / ext4 / ISO9660 drivers are wired together
- [`../hal.md`](../hal.md) — HAL disk + PCI contracts
