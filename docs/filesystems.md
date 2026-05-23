# Filesystems

CanBoot ships four filesystem drivers, each plumbed through a
common `(disk, partition)` dispatch in `cando_port/cando_fs_lib.c`.

| FS | Vendored library | Read | Write | mkfs |
|----|------------------|------|-------|------|
| FAT32   | in-tree (`fs/fat32.c`)         | ✓ | ✓ | ✓ |
| ISO9660 | in-tree (`fs/iso9660.c`)       | ✓ | — | — |
| NTFS    | `vendor/ntfs-3g` (libntfs-3g + mkntfs) | ✓ | ✓ | ✓ |
| ext2/3/4| `vendor/lwext4`                | ✓ | ✓ | ✓ |

## Detection

`fs.detect(disk, part)` reads at most two sectors per partition:

```
LBA 0  ── boot sector ───── + offset 3:  "NTFS"   -> ntfs
                            + offset 54: "FAT"    -> fat16
                            + offset 82: "FAT32"  -> fat32

LBA 2  ── ext superblock ── + offset 56: 0xEF53   -> ext4

LBA 16 ── ISO PVD ────────── + offset 1: "CD001"  -> iso9660
```

Order matters because some signatures overlap. The order above is
fixed in `detect_fs()` in `cando_port/cando_fs_lib.c`.

## FAT32

In-tree driver. Root-directory-only support, 8.3 names. Multi-cluster
chains work for both read and write. Used by the boot media itself.

Format (`fs.mkfs(d, p, "fat32", "LABEL")`) lays down:

- BPB + extended BPB
- Two FATs
- Empty root directory
- Volume serial number from `rdtsc`

## ISO9660

In-tree, read-only. Used for `/init.cdo` discovery on the BIOS / UEFI
ISO when no writable FAT32 disk is attached. Walks the Primary Volume
Descriptor root + path table; doesn't parse Joliet / Rock Ridge
extensions.

## NTFS

Driver from upstream `tuxera/ntfs-3g` vendored at `vendor/ntfs-3g`.
The library expects POSIX (`open`/`pread`/`pwrite` on a block device);
the canboot port reroutes those calls through a custom
`struct ntfs_device_operations` (`cando_port/ntfs3g_canboot_io.c`)
that bridges to the HAL disk read/write at 32-sector batches
(matches virtio-blk's `MAX_BLOCKS_PER_REQ`).

`mkfs` runs the vendored `mkntfs` from `ntfsprogs`. The compile sleeves:

- `cando_port/ntfs3g_canboot_shim.h` is force-included before every
  libntfs source: provides `struct hd_geometry`, `LC_ALL`, etc.
- `cando_port/ntfs3g_mkntfs_main_rename.h` renames `main` ->
  `mkntfs_main_canboot` and overrides the `ntfs_device_default_io_ops`
  macro so mkntfs sees our HAL bridge.
- `cando_port/cando_stubs.c` provides POSIX stubs that mkntfs touches
  on startup (getopt_long, srandom/random, setlocale, etc.).

A `-f` (quick format) flag is always passed so mkntfs skips its
device-wide zero pass.

## ext2 / ext3 / ext4

Driver from `gkostka/lwext4` vendored at `vendor/lwext4`. Build flags
`-DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_DEBUG_PRINTF=0`. The HAL bridge
(`cando_port/lwext4_canboot_io.c`) implements the four-call lwext4
block device interface (open / bread / bwrite / close) over the HAL
disk API.

Each canboot ext4 mount registers a unique device name and mountpoint
(`ext_0` -> `/cb0/`, `ext_1` -> `/cb1/`, ...) so a single boot can
mount up to 4 ext volumes simultaneously.

`fs.mkfs(d, p, "ext4", "LABEL")` calls `ext4_mkfs` directly; we set
`block_size = 4096`, `journal = true`. Known quirk: the free-blocks
counter ends up off by a few thousand vs. what `e2fsck` calculates.
The volume is still well-formed and `e2fsck -fy` repairs the counter
in one pass without touching data.

## Path conventions

The cando `fs.*` API normalises paths so scripts can use the same
shape everywhere:

```
fs.read(1, 0, "/probe.txt")     // forward slash everywhere
fs.read(0, 0, "/init.cdo")
```

For FAT32 the dispatcher strips the leading `/` before calling the
8.3 lookup. For NTFS and ext4 the path is passed through verbatim.

## Test images

The smoke tests build per-FS test images on demand:

| Script | Produces |
|--------|----------|
| `scripts/mkdisk-fat32.sh`        | 64 MiB FAT32, contains `/init.cdo` + `/probe.png` |
| `scripts/mkdisk-ntfs.sh`         | 16 MiB NTFS with `/probe.txt` (uses `mkfs.ntfs` + `ntfscp`) |
| `scripts/mkdisk-ext4.sh`         | 32 MiB ext4 with `/probe.txt` (uses `mkfs.ext4` + `debugfs`) |
| `scripts/mkdisk-aarch64-uefi*.sh`| FAT32 ESP image with the EFI binary + init.cdo |

`scripts/mkdisk-ntfs.sh` and `scripts/mkdisk-ext4.sh` are explicitly
FUSE-free — `ntfscp` and `debugfs` both operate on the image file
directly without mounting, so the test runner doesn't depend on
`/dev/fuse` (which isn't reliably present in CI sandboxes).

## Host-side validation

After the smoke test reformats a volume from canboot, the runner
verifies the result with the host's reference toolchain:

| FS | Host check |
|----|-----------|
| NTFS | `ntfscat -f` reads back the marker file canboot wrote post-format |
| ext4 | `debugfs dump` reads back the marker; `e2fsck -fy` returns 0 after one auto-fix pass |

The canboot-side smoke also re-detects + re-reads through its own FS
driver, so each test exercises the format both end-to-end *and*
cross-validates against the upstream reference.
