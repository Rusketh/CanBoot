# partition — GPT + MBR partition tables

Read **and write** GPT / MBR partition tables. `disk` is the disk
index from [`disk.count()`](disk.md); `idx` is the zero-based partition
index within that disk's table.

## Inspect

### `partition.scheme(disk) -> string`

Partition table format. `"gpt"`, `"mbr"`, or `"none"` (whole-disk
filesystem, no table).

### `partition.count(disk) -> number`

Number of partitions in the table. `0` for `"none"`.

### `partition.start(disk, idx) -> number`

First LBA of partition `idx` (inclusive). `null` for out-of-range.

### `partition.end(disk, idx) -> number`

Last LBA (inclusive).

### `partition.size(disk, idx) -> number`

Length in LBAs (`end - start + 1`).

### `partition.type(disk, idx) -> string`

MBR type byte as 2 hex digits (`"83"` Linux, `"07"` NTFS, `"0c"` FAT32
LBA) or the GPT type GUID. `null` for out-of-range.

### `partition.name(disk, idx) -> string`

GPT partition name (UTF-16 decoded to ASCII); empty for MBR. `null`
for out-of-range.

### `partition.list(disk) -> string`

Newline-separated summary, one line per partition:

```
0  start=2048     size=204800    type=0c  name=
1  start=206848   size=8388608   type=83  name=root
```

## Mutate

⚠ These write the partition table to disk immediately (no "apply"
step). They don't touch filesystem contents, but repartitioning over
a live filesystem makes it unreachable.

### `partition.initGpt(disk) -> bool`

Write a fresh empty GPT (primary at LBA 1, backup at the last LBA,
protective MBR at LBA 0, CRC32s computed). Wipes any existing table.

### `partition.initMbr(disk) -> bool`

Write a fresh empty MBR table (signature `0x55AA`, four zeroed
primaries).

### `partition.create(disk, startLba, endLba, type, name) -> number`

Add a partition spanning `[startLba, endLba]` (inclusive) with `type`
byte (e.g. `131` = 0x83 Linux) and GPT `name` (ignored for MBR).
Returns the new index, or `-1` on failure (table full, overlap, etc.).

```cdo
partition.initGpt(0);
VAR idx = partition.create(0, 2048, 1050623, 131, "root");
```

### `partition.delete(disk, idx) -> bool`

Remove partition `idx`. Other partitions keep their indices (the slot
is zeroed, not compacted).

### `partition.resize(disk, idx, newEndLba) -> bool`

Change partition `idx`'s end LBA (start is fixed). `false` if the new
end overlaps the next partition or runs off the disk.

## Behaviour

- GPT writes keep the primary (LBA 1) + backup (last LBA) headers in
  sync with matching CRC32s, so the result passes `gdisk -l` / Linux
  kernel validation.
- `create` / `resize` validate overlap + disk bounds before writing;
  on rejection the on-disk table is untouched.
- Up to 128 GPT partitions / 4 MBR primaries.
- For "what filesystem is on this partition", see [`fs.detect`](fs.md).

## See also

- [`disk`](disk.md) — raw block device enumeration
- [`fs`](fs.md) — filesystem-aware file ops
