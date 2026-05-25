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

### `disk.writable(i) -> bool`

`true` if the device accepts writes. CD-ROM / read-only media report
`false`.

### `disk.read(i, lba, count) -> string|null`

Read `count` blocks starting at `lba` from disk `i`. Returns the raw
bytes as a binary string (length = `count * blockSize`), or `null` on
error / out-of-range / if the request exceeds the 64 KiB transfer cap.

```cdo
VAR mbr = disk.read(0, 0, 1);          // first sector
print("MBR sig:", hex.encode(mbr));
```

### `disk.write(i, lba, data) -> bool`

Write `data` to disk `i` starting at `lba`. `data`'s length **must be
a whole multiple of the block size** — partial-block writes are
rejected. Capped at 64 KiB per call. Returns `true` on success.

⚠ Raw writes bypass every filesystem. Writing to a live filesystem's
blocks corrupts it.

### Behaviour

- **Transfer cap**: a single `disk.read` / `disk.write` moves at most
  64 KiB. For larger ranges, loop.
- **Block alignment**: writes must be block-multiple-sized; a short
  buffer returns `false` rather than being padded.
- **Binary-safe reads**: `disk.read` returns a length-carrying string,
  so embedded NULs survive — pair with [`hex`](crypto.md#hex) for a
  printable dump.
- Up to 8 disks discoverable. Past 8 the HAL ignores additional
  devices.

---

## partition

Read **and write** GPT / MBR partition tables. `disk` is the disk
index from [`disk.count()`](#disk); `idx` is the zero-based partition
index within that disk's table.

### Inspect

#### `partition.scheme(disk) -> string`

Partition table format. `"gpt"`, `"mbr"`, or `"none"` (whole-disk
filesystem, no table).

#### `partition.count(disk) -> number`

Number of partitions in the table. `0` for `"none"`.

#### `partition.start(disk, idx) -> number`

First LBA of partition `idx` (inclusive). `null` for out-of-range.

#### `partition.end(disk, idx) -> number`

Last LBA (inclusive).

#### `partition.size(disk, idx) -> number`

Length in LBAs (`end - start + 1`).

#### `partition.type(disk, idx) -> string`

MBR type byte as 2 hex digits (`"83"` Linux, `"07"` NTFS, `"0c"` FAT32
LBA) or the GPT type GUID. `null` for out-of-range.

#### `partition.name(disk, idx) -> string`

GPT partition name (UTF-16 decoded to ASCII); empty for MBR. `null`
for out-of-range.

#### `partition.list(disk) -> string`

Newline-separated summary, one line per partition:

```
0  start=2048     size=204800    type=0c  name=
1  start=206848   size=8388608   type=83  name=root
```

### Mutate

⚠ These write the partition table to disk immediately (no "apply"
step). They don't touch filesystem contents, but repartitioning over
a live filesystem makes it unreachable.

#### `partition.initGpt(disk) -> bool`

Write a fresh empty GPT (primary at LBA 1, backup at the last LBA,
protective MBR at LBA 0, CRC32s computed). Wipes any existing table.

#### `partition.initMbr(disk) -> bool`

Write a fresh empty MBR table (signature `0x55AA`, four zeroed
primaries).

#### `partition.create(disk, startLba, endLba, type, name) -> number`

Add a partition spanning `[startLba, endLba]` (inclusive) with `type`
byte (e.g. `131` = 0x83 Linux) and GPT `name` (ignored for MBR).
Returns the new index, or `-1` on failure (table full, overlap, etc.).

```cdo
partition.initGpt(0);
VAR idx = partition.create(0, 2048, 1050623, 131, "root");
```

#### `partition.delete(disk, idx) -> bool`

Remove partition `idx`. Other partitions keep their indices (the slot
is zeroed, not compacted).

#### `partition.resize(disk, idx, newEndLba) -> bool`

Change partition `idx`'s end LBA (start is fixed). `false` if the new
end overlaps the next partition or runs off the disk.

### Behaviour

- GPT writes keep the primary (LBA 1) + backup (last LBA) headers in
  sync with matching CRC32s, so the result passes `gdisk -l` / Linux
  kernel validation.
- `create` / `resize` validate overlap + disk bounds before writing;
  on rejection the on-disk table is untouched.
- Up to 128 GPT partitions / 4 MBR primaries.
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

#### `fs.list(disk, part) -> string`

Newline-separated names of every entry in the partition's root
directory. Empty string if the partition is empty or the FS type
isn't listable.

Currently lists FAT32 (whole-disk) and NTFS roots; ext4 + ISO9660
directory listing isn't wired into this call yet and returns an
empty string.

```cdo
print(fs.list(1, 0));
```

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

Number of PCI functions discovered (all buses / devices / functions).

### `pci.vendor(i) -> string|null`

Vendor ID of function `i` as 4 hex digits (e.g. `"1af4"` virtio,
`"8086"` Intel). `null` for out-of-range.

### `pci.device(i) -> string|null`

Device ID of function `i` as 4 hex digits.

### `pci.class(i) -> string|null`

Class triple `"CC:SC:PI"` — class code, subclass, programming
interface, each 2 hex digits (e.g. `"06:00:00"` host bridge).

### `pci.address(i) -> string|null`

Bus/device/function address as `"BB:DD.F"`.

### `pci.list() -> string`

Newline-separated, one line per function: `address vendor:device class=CC:SC:PI`.

```
00:00.0 1b36:0008 class=06:00:00
00:01.0 1af4:1001 class=01:00:00
00:02.0 1af4:1001 class=01:00:00
00:03.0 1af4:1052 class=09:00:00
00:04.0 1af4:1050 class=03:80:00
00:05.0 1af4:1000 class=02:00:00
```

### Behaviour

- Up to 256 functions enumerated (buses 0..127 / devs 0..31 /
  funcs 0..7).
- Accessors (`vendor` / `device` / `class` / `address`) return `null`
  for an out-of-range index; `pci.list()` dumps everything.
- No BAR / capability inspection at the cando level — that lives in
  the HAL for drivers. The cando surface is "what's on the bus";
  drivers use the C API directly.

## See also

- [`../filesystems.md`](../filesystems.md) — how the FAT32 / NTFS / ext4 / ISO9660 drivers are wired together
- [`../hal.md`](../hal.md) — HAL disk + PCI contracts
