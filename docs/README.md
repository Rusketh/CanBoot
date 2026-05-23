# CanBoot Documentation

CanBoot is a bare-metal bootable runtime for the [CanDo](https://github.com/Rusketh/CanDo)
scripting language. A small freestanding kernel boots on UEFI or BIOS hardware
across x86_64 and aarch64, brings up a HAL (display, input, disk, network,
TLS, audio), and hands control to a CanDo script loaded from disk.

## Quick start

Download the latest artifacts from
[the Releases page](https://github.com/Rusketh/CanBoot/releases/latest)
and run under QEMU:

```sh
# x86_64 BIOS
qemu-system-x86_64 -cdrom canboot-x86_64-bios.iso -serial stdio -display none

# x86_64 UEFI
qemu-system-x86_64 -cdrom canboot-x86_64-uefi.iso \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -serial stdio -display none

# aarch64 -kernel direct
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
    -kernel canboot-aarch64.bin -serial stdio -display none

# aarch64 UEFI
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -display none \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
    -drive if=pflash,format=raw,file=AAVMF_VARS.fd \
    -drive if=none,id=hd0,format=raw,file=canboot-aarch64-uefi.img \
    -device virtio-blk-pci,drive=hd0,bootindex=0 \
    -serial stdio
```

Or boot on real hardware: write the ISO to a USB stick (`dd if=canboot-x86_64-uefi.iso of=/dev/sdX bs=4M`)
or drop the `.efi` onto an existing ESP at `/EFI/BOOT/BOOTX64.EFI` (or
`BOOTAA64.EFI` on aarch64).

## Documentation map

### Getting started

| Doc | What's in it |
|-----|--------------|
| [building.md](building.md) | Building from source. Toolchains, deps, cross-compile setup. |
| [running.md](running.md) | Boot on QEMU + real hardware. Per-platform invocation, serial console hookup. |
| [release.md](release.md) | What's in each release artifact + how to verify signatures. |

### CanDo script API

The cando libraries available to `/init.cdo`. Each library is a global object
with method-style calls.

| Library | What it does |
|---------|--------------|
| [audio](api/audio.md)         | LOVE 2D-shaped Source / mixer / volume / loop (WAV + MP3) |
| [base64](api/base64.md)       | RFC 4648 base64 encode/decode |
| [crypto](api/crypto.md)       | SHA-256 / SHA-512 / HMAC-SHA256 via Mbed TLS |
| [disk](api/disk.md)           | Raw block device enumeration, name, blockSize, blocks |
| [display](api/display.md)     | Framebuffer painter: clear, fillRect, line, text, image, pixel, getPixel |
| [env](api/env.md)             | Boot source + framebuffer + memory-map introspection |
| [fb](api/fb.md)               | Framebuffer flush / present for explicit-scanout devices |
| [file](api/file.md)           | Single-disk root-dir file ops (read/write/list/exists/size) |
| [fmt](api/fmt.md)             | sprintf + binary little-endian packers + sine-wave generator |
| [fs](api/fs.md)               | Filesystem-aware read/write/delete/mkfs (FAT32, NTFS, ext2/3/4) |
| [hex](api/hex.md)             | Hex encode / decode |
| [http](api/http.md)           | HTTP GET (cleartext) over lwIP |
| [https](api/https.md)         | HTTPS GET (TLS 1.2) over Mbed TLS |
| [image](api/image.md)         | PNG / JPG / BMP decode + scaled blit (stb_image) |
| [input](api/input.md)         | Keyboard input poll + blocking waitKey + flush |
| [log](api/log.md)             | Levelled logging with timestamps |
| [net](api/net.md)             | UDP echo + HTTP GET (raw socket-style) |
| [partition](api/partition.md) | GPT + MBR partition table read |
| [pci](api/pci.md)             | PCI bus walk: count, list with vendor/device/class |
| [random](api/random.md)       | RDRAND/ARMv8 RNG + jitter; bytes/int/uuid/hex |
| [time](api/time.md)           | Monotonic clock: ms, us, ticks, sleep |
| [tls](api/tls.md)             | Direct HTTPS GET (alternative entry point to https) |
| [url](api/url.md)             | URL parse: scheme/host/port/path |

### Subsystems

| Doc | What's in it |
|-----|--------------|
| [architecture.md](architecture.md) | Boot flow, address-space layout, how kmain bootstraps the HAL |
| [hal.md](hal.md)                   | HAL surface contracts (audio, disk, display, input, console, net, pci) |
| [bootflow.md](bootflow.md)         | Multiboot2 + UEFI loader handoff to unified kmain |
| [filesystems.md](filesystems.md)   | FAT32, NTFS (libntfs-3g), ext4 (lwext4), ISO9660 |
| [networking.md](networking.md)     | lwIP + virtio-net + Mbed TLS pipeline |
| [audio-stack.md](audio-stack.md)   | Decoder → mixer → HAL → Intel HDA / virtio-sound |

### Contributing

| Doc | What's in it |
|-----|--------------|
| [testing.md](testing.md)       | Running the smoke tests; what each selftest asserts |
| [adding-libs.md](adding-libs.md) | Adding a new cando library |
| [vendoring.md](vendoring.md)   | Adding a new vendored library (stb_image-style or submodule) |

## Status

CanBoot is pre-alpha but functionally substantial:

- Boots cleanly on QEMU x86_64 BIOS, x86_64 UEFI, aarch64 -kernel direct,
  and aarch64 AAVMF UEFI.
- Vendored CanDo, picolibc, lwIP, Mbed TLS, gnu-efi, libntfs-3g, lwext4,
  stb_image, minimp3.
- All 25+ cando libraries above are implemented and exercised by the
  smoke tests on every architecture.
- Single-disk write paths for FAT32, NTFS, ext4 with on-disk-format
  validation via host-side `ntfscat` / `debugfs` / `e2fsck`.
- Real audio output via Intel HDA (x86_64) and virtio-sound (aarch64),
  end-to-end validated by capturing canboot's output to a host WAV and
  asserting audible content.

## License

TBD pending submodule license audit. The vendored libraries carry their
own licenses (gnu-efi BSD, Mbed TLS Apache-2.0, lwIP BSD, picolibc BSD,
stb_image public domain, minimp3 CC0, libntfs-3g LGPL-2.1, lwext4 GPL-2.0,
CanDo TBD).
