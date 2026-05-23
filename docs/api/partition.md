# `partition` — GPT + MBR partition table read

Read-only enumeration. Partition-creation / resize / delete happens at
the C level (`fs/partition.c`); the cando surface exposes only
inspection.

## API

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

## Behaviour

- GPT primary header at LBA 1 is the only one consulted. If the
  primary is corrupted but the backup at the last LBA is valid, this
  API reports `"none"`. The C-level `canboot_part_list` does try the
  backup; the cando surface doesn't expose that yet.
- Up to 128 partitions per disk are enumerated.
- For "what filesystem is on this partition", see [`fs.detect`](fs.md).
