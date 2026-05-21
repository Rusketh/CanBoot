# CanBoot

Bare-metal bootable runtime for the [CanDo](https://github.com/Rusketh/CanDo) language.

CanBoot embeds CanDo as the userspace of a small freestanding kernel that runs
directly on UEFI and BIOS hardware on x86_64 and aarch64. It is designed to
power custom boot tools (PXE installers, recovery shells, kiosk images,
diagnostic environments) written entirely in CanDo.

## Status

Pre-alpha. CanBoot is being built incrementally, one milestone per pull
request.

Landed so far:

- **Milestone 1.** Multiboot2 kernel ELF for x86_64 BIOS: 32 → 64-bit
  long-mode transition, 1 GiB identity-mapped, 16550 UART on COM1, smoke
  test asserts `ok` on serial.
- **Milestone 2.** UEFI PE/COFF loader (`boot/uefi/efi_main.c`) built against
  gnu-efi: boots under QEMU + OVMF, prints firmware vendor + UEFI revision,
  mirrors `ok` on COM1, packaged as a UEFI-only ISO with an embedded ESP
  containing `/EFI/BOOT/BOOTX64.EFI`.
- **Milestone 3.** Unified `kmain(struct boot_info *)`. Both the BIOS and
  UEFI loaders populate the same `boot_info` (framebuffer descriptor, memory
  map, ACPI RSDP, command line) before dispatching the shared kernel code.
  BIOS path parses Multiboot2 tags; UEFI path queries GOP, walks the
  configuration table for the ACPI RSDP, harvests the memory map, and calls
  `ExitBootServices` before transferring control. `kmain` paints the
  framebuffer on both paths to prove access. BIOS bootstrap now
  identity-maps the full 4 GiB so the framebuffer BAR is reachable.

Upcoming milestones bring up the HAL surface (display, input, disk, net,
time, console, fs, entropy), integrate picolibc + lwIP + Mbed TLS, vendor the
CanDo submodule with its patch series, and ship the full set of release
artifacts (hybrid ISO, single-firmware ISOs, PXE bundle, raw `.img`,
standalone `.efi`).

## Building locally

BIOS path:

```
cmake -B build -G Ninja -DCANBOOT_ARCH=x86_64
cmake --build build --target canboot-x86_64
bash scripts/mkiso-bios.sh build/canboot-x86_64.elf build/canboot-x86_64-bios.iso
bash tests/run-qemu-bios.sh build/canboot-x86_64-bios.iso
```

UEFI path:

```
cmake --build build --target canboot-uefi
bash scripts/mkiso-uefi.sh build/canboot-x86_64-uefi.efi build/canboot-x86_64-uefi.iso
bash tests/run-qemu-uefi.sh build/canboot-x86_64-uefi.iso
```

Host build tools required: `gcc`, `cmake`, `ninja-build`, `grub-pc-bin`,
`grub-common`, `gnu-efi`, `ovmf`, `xorriso`, `mtools`, `dosfstools`,
`qemu-system-x86`.

## Layout

| Path | Purpose |
| --- | --- |
| `arch/<arch>/` | Architecture entry, mode transition, MB2 parser, BIOS trampoline |
| `boot/multiboot2/` | Multiboot2 header (requests framebuffer) for BIOS GRUB |
| `boot/uefi/` | PE/COFF EFI loader (gnu-efi); ExitBootServices handoff |
| `kernel/` | Unified `kmain`, `boot_info` schema, framebuffer driver |
| `hal/` | Hardware Abstraction Layer headers + drivers |
| `fs/`, `net/`, `rt/` | VFS, lwIP/Mbed TLS ports, picolibc/pthread shim |
| `vendor/` | Git submodules (CanDo, picolibc, mbedtls, lwip, gnu-efi) |
| `cando_port/` | Patch series + canboot-native CanDo modules |
| `scripts/` | Image-building helpers (ISO, raw, PXE, EFI) |
| `tests/` | QEMU smoke tests |
| `.github/workflows/` | CI + release pipelines |

## License

TBD pending submodule license audit.
