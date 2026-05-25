# `fs` — filesystem-aware partition operations

`fs.*` targets a specific `(disk, partition)` tuple and dispatches by
filesystem type. Read/write/delete works against FAT32, NTFS (via
libntfs-3g), ext2/3/4 (via lwext4) and ISO9660. Format works for
FAT32, NTFS, and ext2/3/4.

Where [file.*](file.md) ignores partitions and walks the root of the
first writable disk, `fs.*` is the one to use for multi-partition
disks and non-FAT32 filesystems.

## Quick recap

```cdo
print("disk 1 part 0 fs =", fs.detect(1, 0));   // -> "ntfs" / "ext4" / "fat32" / ...
print("label =", fs.label(1, 0));
print(fs.read(1, 0, "/probe.txt"));
fs.write(1, 0, "/hello.txt", "world");
fs.delete(1, 0, "/old.txt");

fs.mkfs(1, 0, "ntfs", "MYDISK");                // reformat
```

## Detect + introspect

### `fs.detect(disk, part) -> string`

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

### `fs.label(disk, part) -> string`

Volume label. Works for FAT32 (BPB ext-FS label), NTFS (the unicode
volume name stripped to ASCII), and ext4 (`s_volume_name` from the
superblock). Returns `""` for filesystems without a label or when no
label was set.

### `fs.totalBytes(disk, part) -> number`

Size of the partition in bytes. Computed from `(part.size_lba * block_size)`,
not from any filesystem free-space field. Useful for "how big is this
partition" but not for "how much free space is there".

### `fs.usedBytes(disk, part) -> number`

Currently always `0` — full used-bytes accounting needs per-FS
metadata walks (FAT chain scan, NTFS $Bitmap, ext bitmap aggregation)
that aren't implemented yet. Returns `0` as "unknown" rather than
fabricating a number.

## Read / write / delete

### `fs.read(disk, part, path) -> string|null`

Read a file by path. Binary-safe — the returned cando string carries
the file length as a separate field so embedded NULs survive.

Filesystem-specific behaviour:

| FS | Path convention | Notes |
|----|-----------------|-------|
| FAT32 | Leading `/` is stripped; 8.3 names only; root dir only | Reuses milestone-8 driver |
| NTFS  | Full path (`/dir/file`); leading `/` required | Goes via libntfs-3g; falls back to in-tree read-only driver if mount fails |
| ext4  | Full path (`/dir/file`); leading `/` required | Goes via lwext4 |
| ISO9660 | Bare name; no subdirs | Reuses milestone-8 driver |

Returns `null` for missing files, FS detection failures, etc.

Total buffer is ~64 KiB. Larger files are truncated.

### `fs.write(disk, part, path, data) -> bool`

Write `data` (string) to `path`, replacing any existing file. Returns
`true` on success.

Upsert semantics on NTFS — tries to write to an existing inode first,
falls back to creating a fresh inode with the payload as initial
`$DATA` content.

FAT32 write supports only the root directory. NTFS and ext4 support
nested paths.

### `fs.delete(disk, part, path) -> bool`

Remove the file at `path`. Returns `true` if a file was removed or
`false` if it didn't exist / the FS isn't writable.

Doesn't recurse into directories.

### `fs.list(disk, part) -> string`

Newline-separated names of every entry in the partition's root
directory. Empty string if the partition is empty or the FS type
isn't listable.

Currently lists FAT32 (whole-disk) and NTFS roots. ext4 + ISO9660
directory listing isn't wired into this call yet — they return an
empty string.

```cdo
print(fs.list(1, 0));
```

## Format

### `fs.mkfs(disk, part, type, label) -> bool`

Wipe the partition and lay down a fresh filesystem of `type`:

- `"fat32"` — in-tree formatter
- `"ntfs"` — vendored mkntfs (libntfs-3g/ntfsprogs)
- `"ext2"`, `"ext3"`, `"ext4"` — vendored lwext4
- anything else — returns `false`

The `label` string becomes the volume name in the new filesystem.

Returns `true` if mkfs succeeded.

⚠ **mkfs destroys data on the target partition.** No prompt, no
double-check.

## Behaviour notes

### Multiple mounts

Calling `fs.read` and then `fs.write` on the same NTFS volume in
sequence works correctly — each call mounts, operates, unmounts.
There's no persistent mount handle exposed at the cando level, so two
parallel writes from cando aren't possible (and wouldn't make sense
single-threaded).

### Path separators

Forward slash `/` everywhere — even on FAT32 where the on-disk format
uses backslash semantics. The library strips the leading `/` before
calling FAT32's bare-name lookup.

### Whole-disk partitions

If a disk has no GPT/MBR partition table, `fs.detect/read/write/delete`
all treat `(disk, 0)` as the whole disk. The smoke tests use this for
the FAT32 boot disk and the NTFS / ext4 test images.

## See also

- [partition.md](partition.md) — partition table read (GPT + MBR)
- [disk.md](disk.md)           — raw block device enumeration
- [file.md](file.md)           — single-disk root-dir convenience API
- [filesystems.md](../filesystems.md) — how the FAT32 / NTFS / ext4 / ISO9660 drivers are wired together
