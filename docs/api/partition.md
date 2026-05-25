# `partition` — GPT + MBR partition tables

Read **and write** partition tables. Inspection (scheme / count /
per-field accessors / list) plus mutation (init a fresh GPT or MBR,
create / delete / resize entries). Backed by `fs/partition.c` with
CRC32-correct GPT headers + protective MBR.

`disk` is the disk index from [`disk.count()`](disk.md). `idx` is the
zero-based partition index within that disk's table.

## Inspect

### `partition.scheme(disk) -> string`

Partition table format. `"gpt"`, `"mbr"`, or `"none"` (no recognised
table — whole-disk filesystem).

### `partition.count(disk) -> number`

Number of partitions in the table. `0` for `"none"`.

### `partition.start(disk, idx) -> number`

First LBA of partition `idx` (inclusive). `null` for an out-of-range
index.

### `partition.end(disk, idx) -> number`

Last LBA of partition `idx` (inclusive).

### `partition.size(disk, idx) -> number`

Partition length in LBAs (`end - start + 1`).

### `partition.type(disk, idx) -> string`

Partition type. MBR type byte as 2-hex-digit string (`"83"` for
Linux, `"07"` for NTFS/exFAT, `"0c"` for FAT32 LBA), or the GPT type
GUID. `null` for out-of-range.

### `partition.name(disk, idx) -> string`

GPT partition name (UTF-16 decoded to ASCII). Empty string for MBR
partitions (which have no name field). `null` for out-of-range.

### `partition.list(disk) -> string`

Newline-separated human-readable summary, one line per partition:

```
0  start=2048     size=204800    type=0c  name=
1  start=206848   size=8388608   type=83  name=root
```

```cdo
print(partition.list(0));
```

## Mutate

⚠ All of these write the partition table to disk. They do not touch
filesystem contents — but repartitioning over a live filesystem
makes that filesystem unreachable.

### `partition.initGpt(disk) -> bool`

Write a fresh, empty GPT (primary header at LBA 1, backup at the
last LBA, protective MBR at LBA 0, CRC32s computed). Wipes any
existing partition table. Returns `true` on success.

### `partition.initMbr(disk) -> bool`

Write a fresh, empty MBR partition table (signature `0x55AA`, four
zeroed primary entries). Returns `true` on success.

### `partition.create(disk, startLba, endLba, type, name) -> number`

Add a partition spanning `[startLba, endLba]` (both inclusive) with
the given `type` byte (e.g. `0x83` for Linux). `name` is the GPT
partition name (ignored for MBR). Returns the new partition's index,
or `-1` on failure (table full, overlapping range, etc.).

```cdo
partition.initGpt(0);
VAR idx = partition.create(0, 2048, 1050623, 131, "root");  // 0x83 = 131
print("created partition", idx);
```

### `partition.delete(disk, idx) -> bool`

Remove partition `idx` from the table. Returns `true` on success.
Other partitions keep their indices (the slot is zeroed, not
compacted).

### `partition.resize(disk, idx, newEndLba) -> bool`

Change partition `idx`'s end LBA (its start is fixed). Returns
`true` on success, `false` if the new end would overlap the next
partition or run off the disk.

```cdo
partition.resize(0, 1, 2099199);   // grow partition 1
```

## Behaviour

- GPT writes keep the primary (LBA 1) and backup (last LBA) headers
  in sync with matching CRC32s, so the result passes `gdisk -l` /
  Linux kernel validation.
- `create` / `resize` validate against overlap + disk bounds before
  writing; on rejection the on-disk table is untouched.
- Up to 128 GPT partitions / 4 MBR primaries.
- There's no "apply" step — every mutation writes through to disk
  immediately.

## See also

- [fs.md](fs.md) — format + read/write a filesystem inside a partition
- [disk.md](disk.md) — the underlying block device
