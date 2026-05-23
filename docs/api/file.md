# `file` — single-disk root-directory file ops

A convenience wrapper around the milestone-8 FAT32 driver. Operates on
the first writable disk's root directory. For multi-partition access
or non-FAT32 filesystems use [`fs.*`](fs.md) instead.

## API

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

## Behaviour

- Names follow FAT32 8.3 conventions on disk; the API accepts mixed
  case and either with or without a leading `/`.
- Only the FAT32 driver is exposed. ISO9660 + NTFS + ext4 are reachable
  via [`fs.*`](fs.md) only.
