# CanBoot architecture

A small freestanding kernel that boots on UEFI or BIOS firmware,
brings up enough hardware to host the CanDo language VM, and runs
`/init.cdo` from disk. Hardware bring-up is split into stages
roughly along device lines (console, input, network, TLS, disk,
display, audio, cando VM).

## Bird's-eye view

```
       UEFI firmware                          BIOS + GRUB
            |                                       |
            v                                       v
   boot/uefi/efi_main_*.c               boot/multiboot2/header.S
   (ExitBootServices,                   (32->64 bit transition,
    GOP, mmap, ACPI)                     paging, Multiboot2 parse)
            |                                       |
            +-------- struct boot_info -------------+
                              |
                              v
                  kernel/kmain*.c        (per-arch entry)
                              |
                              | calls into shared kmain_body()
                              v
   +-----------------------------------------------------------+
   |  HAL bring-up (in order):                                 |
   |  - hal_pci_init     -> bus walk, function descriptors    |
   |  - hal_input_init   -> PS/2 + virtio-input               |
   |  - hal_disk_init    -> virtio-blk + AHCI                 |
   |  - hal_net_init     -> virtio-net, lwIP netif + DHCP     |
   |  - hal_display_init -> firmware fb / virtio-gpu          |
   |  - hal_audio_init   -> Intel HDA / virtio-sound          |
   |                                                          |
   |  Boot selftests (tests/selftest/*.c) prove each driver   |
   +-----------------------------------------------------------+
                              |
                              v
                  cando VM: cando_open + cando_openlibs
                  + register all the cando_port/lib/*.c
                  bindings + cando_dostring("/init.cdo")
                              |
                              v
                  Script execution. When the script returns,
                  kmain continues into the input poll loop +
                  audio pump so the system stays interactive.
```

## Address space

### x86_64

Kernel runs at the higher half:

```
0xffffffff80100000   .text + .rodata + .data + .bss
0xffffffff80000000   first 2 GiB of identity-mapped RAM
0x00000000_00000000  whole 4 GiB also identity-mapped via 2 MiB pages
                     (so MMIO BARs and the framebuffer are always
                     reachable without per-driver mmu changes)
```

### aarch64

```
0xffff_0000_4008_0000  kernel virtual base (kernel ELF VMA)
0xffff_0000_0000_0000  identity-mapped peripherals
0x0000_0000_4000_0000  RAM physical base on QEMU virt
```

UEFI loaders do the unfortunate-but-spec-compliant thing where each
section may land at a different runtime offset, so the
`_relocate` function (gnu-efi-derived) walks `.rela` and patches
the GOT before calling our actual `kmain`.

## boot_info

A single struct populated by either loader. Schema lives in
`kernel/include/canboot/boot_info.h`. Everything the unified `kmain`
needs:

| Field | What |
|-------|------|
| `magic`, `version`     | Sanity check |
| `boot_source`          | `BIOS` or `UEFI` or `DIRECT` |
| `cmdline`              | Boot command line |
| `mmap[]`, `mmap_count` | Memory map (usable / reserved / ACPI / etc.) |
| `fb`                   | Framebuffer descriptor (addr, w, h, pitch, channel masks) |
| `acpi_rsdp`            | ACPI root pointer (x86_64) or FDT base (aarch64) |

Loaders fill it in before `ExitBootServices` / before jumping out of
the GRUB stage; the kernel side reads it once and the values are
fixed for the lifetime of boot.

## Subsystem inventory

What's wired today, by layer:

| Layer | Implementations |
|-------|-----------------|
| Boot loader | Multiboot2 (BIOS via GRUB), UEFI PE/COFF (gnu-efi), aarch64 direct-kernel, aarch64 UEFI (AAVMF) |
| Console | 16550 UART (x86_64), PL011 (aarch64) |
| Input | PS/2 i8042, virtio-input |
| Display | Linear framebuffer (firmware-provided), virtio-gpu (aarch64) |
| Disk | virtio-blk, AHCI SATA |
| Net | virtio-net + lwIP 2.2.1 (NO_SYS) |
| Audio | Intel HDA (x86_64), virtio-sound (aarch64) |
| PCI | x86_64 port-IO config space, aarch64 PCIe ECAM |
| Filesystems | ISO9660 (RO), FAT32 (RW root), ext4 via lwext4 (RW + mkfs), NTFS via libntfs-3g (RW + mkntfs) |
| Partition tables | MBR, GPT |
| Runtime | picolibc 1.8.11, cooperative pthread fiber stub, 4 MiB static heap |
| TLS | Mbed TLS 3.6.x LTS with RDSEED/RDRAND + TSC entropy and session tickets |
| Image decode | stb_image (PNG/JPG/BMP) |
| Audio decode | minimp3 (MP3), WAV |
| Language | CanDo VM (full vendored source) with ~25 bare-metal namespaces |

Each subsystem has an in-kernel selftest under `tests/selftest/` that
runs during boot; CI parses serial output for the green markers and
fails the matrix job on any miss.

## Why the cando layer is the only userspace

CanBoot doesn't expose POSIX. The choice was:

- **PROS**: tiny attack surface (single VM, no fork/exec, no syscalls
  outside the cando bindings), one language for everything, scripts
  are portable across BIOS / UEFI / x86_64 / aarch64 without
  recompilation.

- **CONS**: no shell, no traditional process model, no third-party
  binaries. Everything's a `.cdo` script.

That trade is great for "bootable installer / recovery / kiosk image"
use cases. For anything else, you probably want a real OS.

## See also

- [bootflow.md](bootflow.md)       — loader handoff in detail
- [hal.md](hal.md)                 — HAL contracts
- [filesystems.md](filesystems.md) — FAT32 / NTFS / ext4 / ISO9660 plumbing
- [networking.md](networking.md)   — lwIP / Mbed TLS pipeline
- [audio-stack.md](audio-stack.md) — audio decoder + mixer + HAL drivers
