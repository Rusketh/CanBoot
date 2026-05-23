# Running CanBoot

CanBoot ships in five shapes: a Multiboot2 kernel ELF, two UEFI PE images,
and two bootable ISOs. Pick the one that matches your firmware + target
architecture.

## QEMU

The fastest way to try CanBoot. None of these need root.

### x86_64 BIOS

```sh
qemu-system-x86_64 \
    -cdrom canboot-x86_64-bios.iso \
    -serial stdio -display none -no-reboot -m 256M
```

Add `-device virtio-keyboard-pci -netdev user,id=n0 -device virtio-net-pci,netdev=n0`
for the input + networking selftests respectively.

### x86_64 UEFI

```sh
qemu-system-x86_64 \
    -machine q35 \
    -cdrom canboot-x86_64-uefi.iso \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -serial stdio -display none -no-reboot -m 256M
```

The standalone `.efi` boots directly via OVMF if you drop it onto an ESP:

```sh
truncate -s 64M esp.img
mkfs.vfat -F 32 -n CANBOOTEFI esp.img
mmd -i esp.img ::/EFI ::/EFI/BOOT
mcopy -i esp.img canboot-x86_64-uefi.efi ::/EFI/BOOT/BOOTX64.EFI
qemu-system-x86_64 -bios /usr/share/OVMF/OVMF_CODE.fd \
    -drive if=none,id=hd0,format=raw,file=esp.img \
    -device virtio-blk-pci,drive=hd0,bootindex=0 \
    -serial stdio -display none
```

### aarch64 — `-kernel` direct

```sh
qemu-system-aarch64 \
    -machine virt -cpu cortex-a72 -m 512M \
    -kernel canboot-aarch64.bin \
    -serial stdio -display none -no-reboot
```

### aarch64 UEFI (AAVMF)

```sh
cp /usr/share/AAVMF/AAVMF_VARS.fd ./AAVMF_VARS.fd
qemu-system-aarch64 \
    -machine virt -cpu cortex-a72 -m 512M -display none -no-reboot \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
    -drive if=pflash,format=raw,file=AAVMF_VARS.fd \
    -drive if=none,id=hd0,format=raw,file=canboot-aarch64-uefi.img \
    -device virtio-blk-pci,drive=hd0,bootindex=0 \
    -serial stdio
```

## QEMU optional devices

Stack any of these onto the `qemu-system-*` command above to exercise
the corresponding cando library.

### Network

```sh
-netdev user,id=n0 -device virtio-net-pci,netdev=n0
```

SLIRP gives DHCP at `10.0.2.15`, gateway `10.0.2.2`. Host services on
`127.0.0.1:NNNN` are reachable from the guest as `10.0.2.2:NNNN`.

### Sound

x86_64:

```sh
-audiodev wav,id=snd,path=cap.wav -device intel-hda -device hda-duplex,audiodev=snd
```

aarch64:

```sh
-audiodev wav,id=snd,path=cap.wav -device virtio-sound-pci,audiodev=snd
```

After the boot, `cap.wav` holds whatever canboot's audio backend pushed
to the host. You can inspect it with any wave editor.

### Extra block devices

```sh
-drive if=none,id=hd1,format=raw,file=ntfs.img -device virtio-blk-pci,drive=hd1
-drive if=none,id=hd2,format=raw,file=ext4.img -device virtio-blk-pci,drive=hd2
```

Use `scripts/mkdisk-ntfs.sh` and `scripts/mkdisk-ext4.sh` to build them.
Both contain a `/probe.txt` with a known marker.

## Real hardware

### USB stick (BIOS or UEFI)

Write the hybrid ISO to a USB:

```sh
sudo dd if=canboot-x86_64-uefi.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

UEFI x86_64 firmware will boot from `/EFI/BOOT/BOOTX64.EFI` on the ESP
inside the ISO. BIOS-only firmware needs the BIOS ISO:

```sh
sudo dd if=canboot-x86_64-bios.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

aarch64 boards (Raspberry Pi 4 with UEFI firmware, Apple silicon under
Asahi, etc.) take the `canboot-aarch64-uefi.img` directly:

```sh
sudo dd if=canboot-aarch64-uefi.img of=/dev/sdX bs=4M status=progress conv=fsync
```

### Drop into an existing ESP

If your machine already has an EFI system partition, just copy the `.efi`:

```sh
sudo mkdir -p /boot/efi/EFI/canboot
sudo cp canboot-x86_64-uefi.efi /boot/efi/EFI/canboot/canbootx64.efi
# Add a UEFI boot entry pointing at \EFI\canboot\canbootx64.efi
sudo efibootmgr --create --disk /dev/sda --part 1 \
                --label "CanBoot" \
                --loader '\EFI\canboot\canbootx64.efi'
```

### PXE / netboot

The pieces are in place but the netboot bundle isn't currently in the
release. `scripts/mkpxe.sh` (planned) will produce a tarball with kernel
+ initramfs + `dnsmasq.conf` template + `grub-netboot.cfg`. Track via
the issue in the upstream repo.

## Customising `/init.cdo`

The cando script is loaded from the boot media's root as `/init.cdo` on
the first writable FAT32 partition the HAL discovers, falling back to
the ISO9660 root.

Quickest path: drop your script onto a USB stick alongside the
`.efi` / `.iso`. Or rebuild the release locally with `initramfs/init.cdo`
replaced — see [building.md](building.md).

## Serial console

CanBoot prints all of its boot output to:

- **x86_64**: COM1 at I/O port `0x3F8` (16550 UART).
- **aarch64**: PL011 UART at MMIO `0x09000000` (QEMU virt board) or
  whatever the firmware tells us via FDT.

QEMU `-serial stdio` puts the boot log on your terminal. On real
hardware, hook up a USB-to-serial adapter to the appropriate header
(motherboard COM1 / PL011 pins / board-specific debug header).

## What CanBoot does after boot

1. Multiboot2 / UEFI loader fills in `boot_info` (framebuffer, mmap,
   ACPI/FDT pointers) and dispatches `kmain`.
2. `kmain` brings up PCI, input devices, network (DHCP), TLS state,
   block devices, audio.
3. `/init.cdo` is loaded from disk and executed inside the CanDo VM.
4. The script runs to completion. After that, control returns to the
   kernel which sits in an input poll loop forever (so audio + display
   keep working for diagnostic interaction).

See [bootflow.md](bootflow.md) for the wire-level handoff and
[architecture.md](architecture.md) for what each subsystem does.
