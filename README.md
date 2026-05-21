# CanBoot

Bare-metal bootable runtime for the [CanDo](https://github.com/Rusketh/CanDo) language.

CanBoot embeds CanDo as the userspace of a small freestanding kernel that runs
directly on UEFI and BIOS hardware on x86_64 and aarch64. It is designed to
power custom boot tools (PXE installers, recovery shells, kiosk images,
diagnostic environments) written entirely in CanDo.

## Status

Pre-alpha. CanBoot is being built incrementally, one milestone per pull
request. The first milestone establishes the build, packaging, and CI
scaffold: a Multiboot2 kernel that GRUB can load on x86_64 BIOS, prints `ok`
to serial under QEMU, and is shipped as a bootable ISO artifact from the
GitHub Actions workflow.

Upcoming milestones add the UEFI loader, the unified `kmain`, the HAL surface
(display, input, disk, net, time, console, fs, entropy), picolibc + lwIP +
Mbed TLS integration, the CanDo submodule and patch series, and the four
release artifacts (hybrid ISO, single-firmware ISOs, PXE bundle, raw `.img`,
standalone `.efi`).

## Building locally

```
cmake -B build -G Ninja -DCANBOOT_ARCH=x86_64
cmake --build build
bash scripts/mkiso-bios.sh build/canboot-x86_64.elf build/canboot-x86_64-bios.iso
bash tests/run-qemu-bios.sh build/canboot-x86_64-bios.iso
```

Host build tools required: `gcc`, `cmake`, `ninja-build`, `grub-pc-bin`,
`grub-common`, `xorriso`, `mtools`, `qemu-system-x86`.

## Layout

| Path | Purpose |
| --- | --- |
| `arch/<arch>/` | Architecture entry, mode transition, low-level helpers |
| `boot/multiboot2/` | Multiboot2 header for BIOS GRUB |
| `boot/uefi/` | (upcoming) PE/COFF EFI loader |
| `hal/` | Hardware Abstraction Layer headers + drivers |
| `fs/`, `net/`, `rt/` | VFS, lwIP/Mbed TLS ports, picolibc/pthread shim |
| `vendor/` | Git submodules (CanDo, picolibc, mbedtls, lwip, gnu-efi) |
| `cando_port/` | Patch series + canboot-native CanDo modules |
| `scripts/` | Image-building helpers (ISO, raw, PXE, EFI) |
| `tests/` | QEMU smoke tests |
| `.github/workflows/` | CI + release pipelines |

## License

TBD pending submodule license audit.
