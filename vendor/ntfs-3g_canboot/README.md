# ntfs-3g vendoring port — status

This directory holds canboot's overlay onto `vendor/ntfs-3g` (the
tuxera/ntfs-3g submodule). The goal is full r/w NTFS support so the
canboot install/repair tooling can deploy Windows install media and
fix damaged volumes.

**Current status: GROUNDWORK ONLY. NOT YET FUNCTIONAL.**
Do not use this for any operation on a real Windows volume. The
compile pipeline is in place but the runtime path has not been
exercised against a real NTFS volume yet, and the validation against
`chkdsk /f` (which is the actual correctness gate) is its own
follow-up.

## Files in this directory

| File | Purpose |
|---|---|
| `config.h` | Hand-rolled replacement for autoconf's `config.h`. Disables FUSE, POSIX ACLs, xattrs, plugins, crypto, uuid — features that need an OS we don't ship. |
| `README.md` | This file. |

## Files added under `cando_port/`

| File | Purpose |
|---|---|
| `ntfs3g_canboot_shim.h` | Pre-include header. Provides `LC_ALL` stub, `struct hd_geometry` stub, `ntfs_log_handler` typedef forward-decl — fills the gaps picolibc doesn't cover. |
| `ntfs3g_canboot_io.c` | `struct ntfs_device_operations` bridge mapping libntfs's byte-granular pread/pwrite onto our LBA-granular `struct canboot_disk`. Single-sector cache for unaligned access, read-modify-write for partial blocks. |
| `shims/sys/syslog.h` | Stub `<sys/syslog.h>` — declares the symbols `libntfs-3g/ioctl.c` references. |

## Compile coverage (probed locally with shims)

| Status | Files |
|---|---|
| ✅ Compile clean | mst, bootsect, bitmap, index, inode, runlist, logfile, lcnalloc, collate, debug, logging, compat, attrib, mft, dir, misc, volume, unistr, attrlist, security, reparse, ioctl, cache, device, compress, realpath, object_id, xattrs, efs, ea, acls — **32/33 files** |
| ⏭ Skipped (Windows-only) | `win32_io.c` |
| ❌ Not yet | Linking + runtime + CI validation |

## What's NOT yet done

1. **Link integration** — Wiring the 32 compiled object files into the
   kernel + EFI link line is a CMake change of its own. Linker errors
   for libc symbols (snprintf %ll formatting, locale stubs, etc.) need
   case-by-case decisions.
2. **`ntfs_mount()` exercised against the HAL bridge** — never run.
3. **CanDo `fs.write` plumbed through libntfs-3g** — currently still
   returns `false` for NTFS write.
4. **CI test corpus** — no NTFS test image in CI yet; can't validate
   any of the above runs correctly.
5. **`chkdsk /f` validation** — the actual correctness gate. Needs a
   Windows VM in the test loop.

## How to continue this work

Next session / next PR should:

1. Add the 32 libntfs-3g sources + `ntfs3g_canboot_io.c` to the kernel
   + EFI source lists in `CMakeLists.txt`, with the appropriate
   `-include` of `cando_port/ntfs3g_canboot_shim.h`.
2. Fix link errors (`snprintf` format extensions, `gettimeofday`,
   `time`, `localtime` — picolibc may not have all of these, depending
   on the freestanding build flags).
3. Provide a thin C wrapper (`canboot_ntfs3g_open(...)`,
   `canboot_ntfs3g_write_file(...)`) over `ntfs_mount` +
   `ntfs_pathname_to_inode` + `ntfs_attr_pwrite`.
4. Replace `canboot_ntfs_write_root_file` in `fs/ntfs.c` (which
   currently returns -1) with the wrapper.
5. Generate a test NTFS image in CI (`mkfs.ntfs` from the host's
   ntfs-3g package) so the smoke test can exercise the read path
   first, then write path.
6. Run a chkdsk-equivalent post-write check in a Windows-image-mounted
   test stage.

Steps 1-4 are believed to be ~1 PR each. Step 5-6 is its own
infrastructure project.
