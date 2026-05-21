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
- **Milestone 4.** HAL input surface (`hal/include/hal/input.h`) backed by
  a polling pump that drains a shared event ring buffer.
  PS/2 polling driver (`hal/input/ps2.c`) handles the i8042 controller;
  PCI enumeration (`hal/pci/pci_x86.c`) plus the modern virtio-pci
  transport (`hal/virtio/virtio_pci.c`) discover and drive virtio-input
  (`hal/input/virtio_input.c`). `kmain` runs a TSC-bounded pump loop that
  echoes received keystrokes. CI smoke tests attach a virtio-keyboard,
  inject `a b enter` via the QEMU HMP monitor, and assert the full data
  path before `ok`.
- **Milestone 5.** picolibc 1.8.11 vendored at `vendor/picolibc` and
  built via meson + `ExternalProject_Add` with a cross-file targeting
  freestanding x86_64 (`scripts/build-picolibc.sh`,
  `cmake/picolibc-x86_64.cross.in`). POSIX syscall stubs in
  `rt/picolibc_port/syscalls.c` route `write`/`read` through the HAL
  console + input queue and back `sbrk` with a 4 MiB static heap.
  Cooperative pthread stub in `rt/pthread_stub/` provides
  `pthread_create/join/yield/exit`, mutexes, condition variables, and
  `pthread_once` via setjmp/longjmp fibers with a custom first-run
  stack switch. `kernel/m5_selftest.c` exercises printf + malloc +
  string + a 4-worker mutex-protected counter; smoke tests assert
  `milestone 5: self-test ok` before `ok` on both BIOS and UEFI.

Upcoming milestones bring up the rest of the HAL surface (disk, net,
time, fs, entropy), integrate lwIP + Mbed TLS, vendor the CanDo
submodule with its patch series, and ship the full set of release
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

Host build tools required: `gcc`, `cmake`, `ninja-build`, `meson`,
`python3`, `grub-pc-bin`, `grub-common`, `gnu-efi`, `ovmf`, `xorriso`,
`mtools`, `dosfstools`, `qemu-system-x86`.

## Layout

| Path | Purpose |
| --- | --- |
| `arch/<arch>/` | Architecture entry, mode transition, MB2 parser, BIOS trampoline |
| `boot/multiboot2/` | Multiboot2 header (requests framebuffer) for BIOS GRUB |
| `boot/uefi/` | PE/COFF EFI loader (gnu-efi); ExitBootServices handoff |
| `kernel/` | Unified `kmain`, `boot_info` schema, framebuffer driver |
| `hal/` | Hardware Abstraction Layer headers + drivers |
| `fs/`, `net/`, `rt/` | VFS (later), lwIP/Mbed TLS ports (later), picolibc syscall stubs, cooperative pthread stub |
| `vendor/` | Git submodules — picolibc today; CanDo, mbedtls, lwip, gnu-efi to come |
| `cando_port/` | Patch series + canboot-native CanDo modules |
| `scripts/` | Image-building helpers (ISO, raw, PXE, EFI) |
| `tests/` | QEMU smoke tests |
| `.github/workflows/` | CI + release pipelines |

## License

TBD pending submodule license audit.
