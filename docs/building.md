# Building CanBoot from source

CanBoot uses CMake + Ninja. There are two build trees — one per architecture —
both driven from the same `CMakeLists.txt`.

## Host requirements

CanBoot is cross-built from a Linux host. macOS is not currently tested.

### Common

- `cmake` ≥ 3.20, `ninja-build`, `make`
- `meson` ≥ 0.62, `python3`
- `gcc` (host) for the x86_64 build
- `gcc-aarch64-linux-gnu` for the aarch64 build
- `xorriso`, `mtools`, `dosfstools` for ISO + ESP image generation
- `grub-pc-bin`, `grub-common`, `gnu-efi` for the BIOS / UEFI boot stages
- `ovmf`, `qemu-efi-aarch64` for UEFI firmware images (smoke tests only)
- `qemu-system-x86`, `qemu-system-arm`, `qemu-system-misc` for smoke tests

### Filesystem test extras

The smoke tests build NTFS + ext4 disk images and validate canboot writes via
host-side tooling:

- `ntfs-3g` — provides `mkfs.ntfs`, `ntfscp`, `ntfscat` for the NTFS path.
- `e2fsprogs` — provides `mkfs.ext4`, `debugfs`, `e2fsck` for the ext4 path.

Debian / Ubuntu, all in one:

```sh
sudo apt-get install -y --no-install-recommends \
    cmake ninja-build gcc binutils make meson python3 \
    gcc-aarch64-linux-gnu \
    grub-pc-bin grub-common gnu-efi ovmf qemu-efi-aarch64 \
    qemu-system-x86 qemu-system-arm qemu-system-misc \
    xorriso mtools dosfstools \
    ntfs-3g e2fsprogs
```

## Cloning

```sh
git clone --recurse-submodules https://github.com/Rusketh/CanBoot.git
cd CanBoot
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

Submodules pulled in:

| Submodule | What it is |
|-----------|------------|
| `vendor/cando`     | The CanDo scripting language |
| `vendor/picolibc`  | Freestanding C runtime |
| `vendor/lwip`      | TCP/IP stack |
| `vendor/mbedtls`   | TLS 1.2 |
| `vendor/gnu-efi`   | UEFI boot stubs |
| `vendor/ntfs-3g`   | NTFS read/write + mkntfs |
| `vendor/lwext4`    | ext2/3/4 read/write + mkfs |
| `vendor/stb`       | Single-header stb_image for PNG/JPG/BMP |
| `vendor/minimp3`   | Single-header MP3 decoder |

## x86_64 BIOS

```sh
cmake -B build -G Ninja -DCANBOOT_ARCH=x86_64
cmake --build build --target canboot-x86_64
bash scripts/mkiso/bios.sh build/canboot-x86_64.elf build/canboot-x86_64-bios.iso
```

Artifacts produced:

| File | Description |
|------|-------------|
| `build/canboot-x86_64.elf`      | Multiboot2 kernel ELF |
| `build/canboot-x86_64-bios.iso` | Bootable BIOS ISO (GRUB stage + kernel + initramfs) |

Smoke test:

```sh
bash tests/run-qemu-bios.sh build/canboot-x86_64-bios.iso
```

## x86_64 UEFI

Add to the existing build tree:

```sh
cmake --build build --target canboot-uefi
bash scripts/mkiso/uefi.sh build/canboot-x86_64-uefi.efi build/canboot-x86_64-uefi.iso
```

Artifacts produced:

| File | Description |
|------|-------------|
| `build/canboot-x86_64-uefi.efi` | PE/COFF UEFI application |
| `build/canboot-x86_64-uefi.iso` | Bootable UEFI ISO (ESP + EFI/BOOT/BOOTX64.EFI) |

Smoke test:

```sh
bash tests/run-qemu-uefi.sh build/canboot-x86_64-uefi.iso
```

## aarch64

Separate build tree because of the cross-toolchain:

```sh
cmake -B build-aarch64 -G Ninja \
    -DCANBOOT_ARCH=aarch64 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
```

### Direct (`-kernel`)

```sh
cmake --build build-aarch64 --target canboot-aarch64-bin
```

Artifacts:

| File | Description |
|------|-------------|
| `build-aarch64/canboot-aarch64.elf` | Kernel ELF |
| `build-aarch64/canboot-aarch64.bin` | Flat binary, no ELF wrapper, drop-in for `qemu -kernel` |

Smoke test:

```sh
bash tests/run-qemu-aarch64.sh build-aarch64/canboot-aarch64.bin
```

### UEFI

```sh
cmake --build build-aarch64 --target canboot-uefi
bash scripts/mkdisk/aarch64-uefi.sh \
    build-aarch64/canboot-aarch64-uefi.efi \
    build-aarch64/canboot-aarch64-uefi.img
```

Artifacts:

| File | Description |
|------|-------------|
| `build-aarch64/canboot-aarch64-uefi.efi` | PE/COFF UEFI application |
| `build-aarch64/canboot-aarch64-uefi.img` | Raw FAT32 ESP image with `/EFI/BOOT/BOOTAA64.EFI` |

Smoke test:

```sh
bash tests/run-qemu-aarch64-uefi.sh build-aarch64/canboot-aarch64-uefi.img
```

## What gets built where

| Target              | Produces                                       |
|---------------------|------------------------------------------------|
| `canboot-x86_64`    | `build/canboot-x86_64.elf` (Multiboot2 kernel) |
| `canboot-uefi`      | Both arches' `*-uefi.efi` |
| `canboot-aarch64-bin` | `build-aarch64/canboot-aarch64.{elf,bin}` |
| `canboot-info`      | Prints kernel ELF path (used by build pipeline) |

## Customising the init script

`initramfs/init.cdo` is the cando script embedded on the boot media.
To ship your own, edit `initramfs/init.cdo` in place and rebuild the disk
image — `scripts/mkdisk/fat32.sh`, `scripts/mkdisk/aarch64-uefi.sh`,
`scripts/mkiso/{bios,uefi}.sh` all pick it up at packaging time.

## Build flags

| Flag | Purpose |
|------|---------|
| `-DCANBOOT_ARCH=<x86_64\|aarch64>` | Target architecture. Required. |
| `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake` | Required for aarch64. |
| `-DCMAKE_BUILD_TYPE=Debug` | Adds `-g -O0`; default is `Release`. |

## Troubleshooting

**`picolibc` build fails with meson errors** — usually a stale cross-file.
Delete `build*/picolibc/` and rebuild. The wrapper script auto-handles
the "build dir exists" case via `--reconfigure`.

**`gnu-efi` build can't find headers** — install `gnu-efi` host package
(Debian/Ubuntu) so the wrapped Makefile can pick up `efi.h`.

**`xorriso` complains about missing GRUB stages** — install
`grub-pc-bin` + `grub-common`.

**aarch64 build fails with `aarch64-linux-gnu-gcc: command not found`** —
install `gcc-aarch64-linux-gnu`.

**QEMU smoke tests time out** — most often the host doesn't have
`qemu-system-x86_64` / `qemu-system-aarch64` on PATH, or sidecar python
servers (`tests/sidecars/*.py`) couldn't bind to a port. Check
`build*/qemu-*.log` and `build*/qemu-*.stderr.log` for clues.

## CI

GitHub Actions runs the same flow on every push: build all four targets,
run the QEMU smoke tests, upload artifacts. See `.github/workflows/ci.yml`.
Release artifacts are auto-published; see [release.md](release.md).
