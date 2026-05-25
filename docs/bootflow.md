# Boot flow

CanBoot has four loaders, all of which converge on a single
`kmain(struct boot_info *)`:

1. **x86_64 BIOS** — GRUB chainloads our Multiboot2 ELF.
2. **x86_64 UEFI** — firmware loads our PE/COFF image directly.
3. **aarch64 UEFI** — AAVMF / EDK2 loads our PE/COFF image.
4. **aarch64 direct** — QEMU's `-kernel` loads the flat `.bin`.

## x86_64 BIOS path

```
Firmware POST -> GRUB stage 1.5 -> GRUB stage 2 reads
boot/grub/grub.cfg from the ISO -> chainload canboot.elf
via Multiboot2.

boot/multiboot2/header.S declares the MB2 magic + flags:
  - framebuffer request (any depth, any resolution)
  - module alignment

arch/x86_64/bootstrap.S receives control in protected mode (32-bit):
  - sets up identity-mapped page tables for the first 4 GiB
    (PML4 -> PDPT -> 2 MiB PDs)
  - enables PAE + long mode (CR4.PAE, EFER.LME, CR0.PG)
  - jumps into 64-bit code

arch/x86_64/bios_entry.c (now in 64-bit long mode):
  - parses MB2 tags via arch/x86_64/mb2_parse.c
  - populates boot_info from MB2 fb tag + mmap + cmdline tags
  - calls kmain(&boot_info)
```

## x86_64 UEFI path

```
Firmware loads /EFI/BOOT/BOOTX64.EFI from the ESP.

boot/uefi/efi_main.c entry:
  - InitializeLib (gnu-efi runtime initialisation)
  - LocateProtocol(GOP) for the framebuffer descriptor
  - Walk the EFI configuration table for the ACPI 2.0 RSDP
  - GetMemoryMap (typically twice — once to size, once to populate)
  - ExitBootServices(image, map_key) (retried once if the map shifted)
  - call kmain(&boot_info)
```

`kmain()` (kernel/kmain.c) immediately switches off the firmware stack
onto a static 256 KiB stack — the firmware stack is too small for
Mbed TLS's >150 KiB TLS-handshake frames — before running any bring-up.

## aarch64 paths

The UEFI path mirrors x86_64's almost exactly — same gnu-efi-derived
entry, GOP / mmap / ACPI-or-FDT lookups, ExitBootServices — then
allocates a 32 MiB stack via `AllocatePages`, switches `sp` onto it,
and calls `kmain(&boot_info)` (boot/uefi/efi_main_aarch64.c).

The direct (`-kernel`) path skips all of that. QEMU honours the
Linux/ARM64 boot protocol, loading the flat `.bin` at physical
`0x40080000` (RAM base `0x40000000` + the 0x80000 `text_offset` in the
boot header) with the **MMU off** — no virtual mapping:

```
arch/aarch64/bootstrap.S:
  - stash the FDT pointer (x0 at entry) in x19
  - if started at EL2 (QEMU's default), drop to EL1 via the
    ERET trick (set HCR_EL2.RW, SPSR/ELR, eret)
  - enable EL1 FP/SIMD access (picolibc uses NEON)
  - set sp to top of the static main stack
  - zero .bss
  - restore the FDT pointer into x0 and bl aarch64_kmain_entry

arch/aarch64/aarch64_entry.c:
  - parse FDT via arch/aarch64/fdt.c (memory layout into boot_info.mmap)
  - stash the FDT pointer in boot_info.acpi_rsdp
  - call kmain(&boot_info)
```

## Convergence point

All four loaders call the single entry symbol
`kmain(struct boot_info *)`, which has two arch-specific definitions:
`kernel/kmain.c` (x86_64) and `kernel/kmain_aarch64.c` (aarch64). Each
switches onto a large dedicated stack first, then runs the same
bring-up sequence — the x86_64 path factors the body into a static
`kmain_body()`; the aarch64 path inlines it. The sequence is identical
across arches except for arch-conditional sections that handle GDT/IDT
(x86_64 only) and virtio-gpu fallback (aarch64 only, when AAVMF doesn't
expose GOP).

`kmain` then runs the bring-up stages in order:

| Step | What |
|------|------|
| Validate `boot_info` magic + version | |
| Print boot summary on serial | "canboot: kmain reached" etc. |
| Init HAL: PCI -> input -> disk -> net -> display -> audio | |
| Run all selftests under tests/selftest/ | proves each driver |
| Init CanDo VM | `cando_open` + `cando_openlibs` |
| Register all cando_port/lib/*.c bindings | |
| Load `/init.cdo` from FAT32 / ISO9660 | |
| `cando_dostring(init_src)` | script runs |
| Enter input pump loop | audio + input stay live |

## What you see on serial

Roughly this, in order:

```
canboot: uefi entry reached (aarch64)   (only on UEFI)
canboot: calling ExitBootServices (aarch64)
canboot: kmain reached (aarch64)
canboot: boot_info v1 source=uefi
canboot: fb rgb addr=0x40000000 1024x768x32
canboot: mmap entries=36
canboot: usable bytes=0x000000001fa70000
canboot: platform-tables=0x...
canboot: pci devs=6
canboot: virtio-input present
canboot: input loop start
... selftest output ...
selftest: image+audio libs registered
selftest: --- init.cdo output begin ---
canboot-cando-runtime-marker init.cdo executed inside vm
... init.cdo output ...
selftest: --- init.cdo output end ---
selftest: cando_dostring ok rc=0
canboot: aarch64 hello world boot complete
ok
```

The trailing `ok` is the canary every test runner looks for to know
the boot reached steady-state.
