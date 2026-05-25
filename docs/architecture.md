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
                  shared kmain() entry   (per-arch: kernel/kmain.c,
                              |            kernel/kmain_aarch64.c)
                              | switches to a large boot stack,
                              | then runs the bring-up body
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

Both architectures run with a flat (identity) address space — there's
no higher-half kernel and no per-driver remapping, so MMIO BARs and the
framebuffer are reachable at their physical addresses throughout.

### x86_64

`arch/x86_64/bootstrap.S` enters long mode with the **first 4 GiB
identity-mapped via 2 MiB pages** (PML4[0] → PDPT[0..3] → four page
directories, each entry `present | writable | PS`).

```
0x00000000_00100000  kernel .text + .rodata + .data + .bss
                     (KERNEL_LOAD_ADDR, linker/kernel.x86_64.ld)
0x00000000_00000000  whole 4 GiB identity-mapped via 2 MiB pages
                     (MMIO BARs + framebuffer reachable directly)
```

### aarch64

The direct `-kernel` path (`arch/aarch64/bootstrap.S`) drops from EL2
to EL1, enables FP/SIMD (picolibc needs it), and runs with the **MMU
left off** — flat physical addressing.

```
0x00000000_40080000  kernel image (RAM base + 0x80000 text_offset,
                     linker/kernel.aarch64.ld)
0x00000000_40000000  RAM physical base on QEMU virt
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
| `magic`, `version`     | Sanity check (`'CNBT'`, schema v1) |
| `boot_source`          | `CANBOOT_BOOT_BIOS_MB2`, `CANBOOT_BOOT_UEFI`, or `CANBOOT_BOOT_UNKNOWN` (the aarch64 direct-kernel path reuses `BIOS_MB2`) |
| `flags`                | Reserved for future use |
| `fb`                   | Framebuffer descriptor (addr, w, h, pitch, bpp, channel masks) |
| `mmap[]`, `mmap_count` | Memory map (usable / reserved / ACPI / etc.) |
| `acpi_rsdp`            | ACPI RSDP (x86_64) or FDT base (aarch64) |
| `cmdline_phys`         | Physical address of the boot command line |

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
| Runtime | picolibc 1.8.11, preemptive-capable thread scheduler (rt/sched) behind the pthread surface, 4 MiB static heap |
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
