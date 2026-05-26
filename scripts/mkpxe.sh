#!/usr/bin/env bash
# Build a PXE/netboot tree for one arch+firmware target. Produces, under the
# given output directory:
#
#   <out>/tftp/            files a TFTP server hands to the PXE client
#   <out>/dnsmasq.conf     a ready-to-edit DHCP+TFTP server template
#   <out>/PXE-README.txt   how to stand the server up
#
# The CanBoot kernel learns the TFTP server from DHCP (BOOTP siaddr / option
# 66 / option 150) and pulls /init.cdo from it (net/tftp.c), so init.cdo is
# placed at the TFTP root here.
#
# Usage: mkpxe.sh <arch> <firmware> <binary> <out_dir>
#   arch:     x86_64 | aarch64
#   firmware: bios | uefi | direct
#   binary:   kernel ELF (bios) / .efi (uefi) / flat .bin (direct)

set -euo pipefail

ARCH="${1:?arch required (x86_64|aarch64)}"
FW="${2:?firmware required (bios|uefi|direct)}"
BIN="${3:?boot binary required}"
OUT="${4:?output dir required}"

if [ ! -f "$BIN" ]; then
    echo "error: boot binary not found at $BIN" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INIT_CDO="$ROOT_DIR/initramfs/init.cdo"
GUI_CDO="$ROOT_DIR/modules/gui/gui.cdo"
PROBE_PNG="$ROOT_DIR/initramfs/probe.png"

TFTP="$OUT/tftp"
rm -rf "$TFTP"
mkdir -p "$TFTP"

# The boot script + its companions live at the TFTP root; the kernel requests
# them by basename over TFTP.
[ -f "$INIT_CDO" ]  && cp "$INIT_CDO"  "$TFTP/init.cdo"
[ -f "$GUI_CDO" ]   && cp "$GUI_CDO"   "$TFTP/gui.cdo"
[ -f "$PROBE_PNG" ] && cp "$PROBE_PNG" "$TFTP/probe.png"

NBP=""   # DHCP boot filename (option 67) the firmware downloads

case "$ARCH/$FW" in
    x86_64/uefi)
        cp "$BIN" "$TFTP/BOOTX64.EFI"
        NBP="BOOTX64.EFI"
        ;;
    aarch64/uefi)
        cp "$BIN" "$TFTP/BOOTAA64.EFI"
        NBP="BOOTAA64.EFI"
        ;;
    x86_64/bios)
        # GRUB's PXE core + modules, then our kernel + a Multiboot2 menu that
        # mirrors the BIOS ISO config (scripts/mkiso/bios.sh).
        mkdir -p "$TFTP/boot/grub"
        cp "$BIN" "$TFTP/boot/canboot.elf"
        cat > "$TFTP/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

menuentry "canboot" {
    multiboot2 /boot/canboot.elf
    boot
}
EOF
        grub-mknetdir --net-directory="$TFTP" --subdir=boot/grub >/dev/null
        NBP="boot/grub/i386-pc/core.0"
        ;;
    aarch64/direct)
        # No standard network bootloader for the bare -kernel path; ship the
        # flat image for a u-boot / UEFI front-end to fetch and launch.
        cp "$BIN" "$TFTP/canboot-aarch64.bin"
        NBP="canboot-aarch64.bin"
        ;;
    *)
        echo "error: unsupported target $ARCH/$FW" >&2
        exit 1
        ;;
esac

cat > "$OUT/dnsmasq.conf" <<EOF
# CanBoot PXE/netboot - dnsmasq template ($ARCH/$FW).
# Edit the interface and DHCP range for your network, then:
#   sudo cp -r tftp/* /srv/tftp/canboot/
#   sudo dnsmasq -C dnsmasq.conf -d
#
# CanBoot reads /init.cdo from this TFTP server, discovered from the DHCP
# siaddr (next-server, which dnsmasq sets to this host).

interface=eth0
bind-interfaces
dhcp-range=192.168.50.50,192.168.50.150,12h
enable-tftp
tftp-root=/srv/tftp/canboot

# Boot file handed to the client (DHCP option 67).
dhcp-boot=$NBP
EOF

cat > "$OUT/PXE-README.txt" <<EOF
CanBoot PXE/netboot ($ARCH/$FW)
===============================

Contents
  tftp/          serve this directory's contents from your TFTP root
  dnsmasq.conf   DHCP + TFTP server template (edit interface/range)

Steps
  1. Install dnsmasq (or use an existing DHCP+TFTP setup).
  2. Copy the tftp/ contents into your TFTP root, e.g.
        sudo mkdir -p /srv/tftp/canboot
        sudo cp -r tftp/* /srv/tftp/canboot/
  3. Point dnsmasq at it and start it:
        sudo dnsmasq -C dnsmasq.conf -d
  4. Net-boot the client. Firmware downloads "$NBP", CanBoot comes up, learns
     the TFTP server from DHCP, and fetches /init.cdo over TFTP. With no
     server it falls back to local media.
EOF

echo "wrote $OUT/tftp ($ARCH/$FW, NBP=$NBP)"
