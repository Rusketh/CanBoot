# CanBoot architecture

A small freestanding kernel that boots on UEFI or BIOS firmware,
brings up enough hardware to host the CanDo language VM, and runs
`/init.cdo` from disk. Hardware bring-up is split into "milestones"
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
   |  Milestone self-tests (mNN_*test.c) prove each driver    |
   +-----------------------------------------------------------+
                              |
                              v
                  cando VM: cando_open + cando_openlibs
                  + register all the cando_port/cando_*_lib.c
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

## Milestones (chronological)

The repo is organised by milestone for the same reason the
[plan](https://github.com/Rusketh/CanBoot/blob/main/docs/plan.md)
is: each landed PR exercises one HAL surface end-to-end before
moving on, so failures pin to a small recent diff.

| # | Subsystem |
|---|-----------|
| 1 | Multiboot2 boot + 16550 UART |
| 2 | UEFI loader + GOP framebuffer |
| 3 | Unified kmain, framebuffer paint |
| 4 | HAL input (PS/2 + virtio-input) |
| 5 | picolibc + pthread stub |
| 6 | lwIP + virtio-net + DHCP / HTTP |
| 7 | Mbed TLS + HTTPS |
| 8 | HAL disk + FAT32 / ISO9660 |
| 9 | CanDo VM linked into kernel |
| 10 | cando_dostring("/init.cdo") |
| 11 | cando display.* + GOP self-test |
| 12 | cando input.* + waitKey loop |
| 13 | aarch64 boot + PL011 + cross-build |
| 14 | aarch64 UEFI parity |
| 15-17 | aarch64 milestone-5..10 parity (picolibc / lwIP / Mbed TLS / disk / cando / display / input) |
| 18 | NTFS read+write (libntfs-3g) + mkntfs |
| 19 | ext4 read+write (lwext4) + mkfs |
| 20 | image + audio (stb_image / minimp3 / Intel HDA / virtio-sound / LOVE-style mixer) |

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
