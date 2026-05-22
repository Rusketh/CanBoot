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
- **Milestone 6.** lwIP 2.2.1 vendored at `vendor/lwip` and compiled
  into the kernel from its `Filelists.cmake` source lists. NO_SYS=1
  mode with `net/lwip_port/sys_arch.c` providing `sys_now()` backed by
  rdtsc + an i8254-PIT TSC calibration. Modern virtio-net driver
  (`hal/net/virtio_net.c`, transitional and non-transitional device
  IDs supported) wires into lwIP as a `NETIF_FLAG_ETHARP` netif.
  `kernel/m6_nettest.c` runs DHCP (10.0.2.x lease from QEMU SLIRP) +
  UDP echo + HTTP GET against sidecar Python servers spawned by the
  smoke test on the host's loopback. `kmain` enables SSE/SSE2 state
  bits in CR0/CR4 so picolibc + lwIP can use SSE-emitting paths
  without tripping #UD.
- **Milestone 7.** Mbed TLS 3.6.6 (LTS) vendored at `vendor/mbedtls`
  and brought in via `add_subdirectory(EXCLUDE_FROM_ALL)`; the
  user-config in `net/mbedtls_port/include/canboot_mbedtls_user_config.h`
  trims POSIX dependencies. A hardware entropy hook
  (`net/mbedtls_port/entropy.c`) tries RDSEED/RDRAND (CPUID-gated to
  avoid #UD on `qemu64`) then falls back to a TSC-jitter mixer; a
  cooperative TCP BIO (`net/mbedtls_port/lwip_bio.c`) wraps lwIP's
  raw API into the synchronous `mbedtls_ssl_send`/`recv` shape Mbed
  TLS expects. `kernel/m7_tlstest.c` performs the full TLS 1.2
  handshake against a sidecar Python HTTPS server
  (`tests/sidecars/https_secure.py`) using a pinned self-signed CA
  at `tests/sidecars/tls/canboot-test.pem` (embedded in the kernel via
  `kernel/canboot_test_ca.c`, regenerable with
  `scripts/embed-test-ca.sh`), validates the chain, issues an HTTPS
  GET, then reconnects to verify session-ticket resumption (~25x
  faster handshake). Runs on both BIOS and UEFI.
- **Milestone 9.** CanDo vendored at `vendor/cando` (submodule of
  `Rusketh/CanDo`). A curated subset of `vendor/cando/source/*.c`
  (~50 files; everything except the OpenSSL/socket/HTTP libs we'll
  swap for Mbed TLS + lwIP bindings in a later milestone) is compiled
  directly into both kernel and EFI builds, with bare-metal header
  shims in `cando_port/shims/` (openssl, netdb, netinet, sys/mman,
  sys/socket, sys/ioctl, sys/utsname, sys/sysinfo, dirent, dlfcn,
  termios) and POSIX function stubs in `cando_port/cando_stubs.c`.
- **Milestone 13 (first cut).** aarch64 kernel boots to `ok` on a
  PL011 UART under `qemu-system-aarch64 -machine virt`. Cross-build
  via `gcc-aarch64-linux-gnu` driven by
  `cmake/toolchain-aarch64.cmake`. `arch/aarch64/bootstrap.S` handles
  the EL2 → EL1 drop, zeroes `.bss`, sets the stack, and dispatches
  `aarch64_kmain_entry`. `hal/console/serial_aarch64.c` polls the
  PL011 FR.TXFF flag at the well-known QEMU virt MMIO base
  (`0x09000000`). The aarch64 kernel is intentionally minimal in this
  PR (no picolibc / lwIP / mbedtls / cando yet) so we have a green
  CI signal for the cross-toolchain pipeline; subsequent aarch64
  PRs port milestones 2-12 onto the same boot path.
- **Milestone 12.** CanDo-facing input module. `cando_port/cando_input_lib.c`
  exposes `input.{poll, waitKey, flush, events}` on top of the
  milestone-4 HAL input queue; `waitKey` cooperatively pumps the HAL
  devices for up to the requested ms, returning the key's ASCII code
  or null on timeout. `init.cdo` now extends past the display painting
  into an input phase: it flushes any stale events, calls
  `input.waitKey(8000)` three times, and prints each received key.
  CI smoke tests already injected keystrokes for the milestone-4
  pre-cando phase; a second injection wave triggered by the "cando
  input poll begin" marker sends `x y z` so the cando script receives
  them and the serial log carries `cando got key1: 120` / `121` / `122`.
- **Milestone 11.** CanDo-facing display module. `hal/include/hal/display.h`
  + `hal/display/display.c` extend the milestone-3 framebuffer painter
  with pixel/line/text/image/copyRect/getPixel primitives and an 8x8
  ASCII bitmap font. `cando_port/cando_display_lib.c` registers them
  as a `display.*` cando namespace via `cando_bridge_new_object` +
  `libutil_register_methods` + `cando_vm_set_global`. `init.cdo` now
  drives the framebuffer through cando, painting three RGB rectangles
  + a diagonal line + a text label; kmain reads back known pixel
  coordinates and asserts exact colours. CI smoke tests additionally
  `screendump` the QEMU framebuffer over HMP after the paint marker
  and assert the full PPM's SHA256 matches a checked-in reference
  (`tests/refs/m11-{bios,uefi}.ppm.sha256`). Different references per
  firmware because GOP gives UEFI 1280x800 while GRUB hands BIOS
  1024x768.
- **Milestone 10 (plan's primary milestone).** The CanDo VM runs on
  bare metal. `kmain` now installs a minimal IDT
  (`arch/x86_64/idt.{c,h}`, `arch/x86_64/idt_stubs.S`) that prints
  trap frames on any CPU exception, sets up FS_BASE against a static
  16 KiB TLS area for cando's `_Thread_local` variables, drops
  `-fcf-protection` so gcc stops emitting `endbr64` (QEMU's `qemu64`
  CPU lacks CET and treats it as #UD), and calls
  `cando_open` → `cando_openlibs` → `cando_dostring(/init.cdo
  contents)` → `cando_close`. `/init.cdo` is loaded via milestone 8's
  FAT32+ISO9660 path and executes inside the VM; its `print()` lands
  on serial via picolibc → `hal_console`. Runs on both BIOS and UEFI.
  The UEFI link was unblocked by `-D_Thread_local=` (defangs cando's
  four `_Thread_local` statics so we don't get General-Dynamic TLS
  relocations needing `__tls_get_addr`) plus adding `libgcc.a` to the
  EFI link so picolibc/Mbed TLS's 128-bit-divide builtins resolve.
- **Milestone 8.** HAL disk surface (`hal/include/hal/disk.h`,
  `hal/disk/disk.c`) plus three drivers wired underneath: virtio-blk
  (`hal/disk/virtio_blk.c`, reuses milestone 4's virtio-pci
  transport) and AHCI SATA (`hal/disk/ahci.c`, BAR5 ABAR, 32-slot
  command list, LBA48 READ/WRITE DMA EXT). Filesystems:
  read-only ISO9660 (`fs/iso9660.c`) for booting from the canboot ISO
  and read+write FAT32 (`fs/fat32.c`) for an attached disk image (root
  directory only, 8.3 names, multi-cluster chains). `kmain` runs
  `kernel/m8_disktest.c`: discover all block devices, look up
  `/init.cdo` on a writable FAT32 disk first (round-tripping a small
  write-probe to verify the write path), fall back to the ISO9660
  root, and assert a build-time `canboot-init-marker` in the loaded
  body. Smoke tests build a 64 MiB FAT32 disk image carrying
  `initramfs/init.cdo` (`scripts/mkdisk-fat32.sh`) and attach it to
  QEMU via virtio-blk.

Upcoming milestones vendor the CanDo submodule with its patch series
and start linking `libcando.a` into the kernel, then ship the full set
of release artifacts (hybrid ISO, single-firmware ISOs, PXE bundle,
raw `.img`, standalone `.efi`).

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
