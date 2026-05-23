#!/usr/bin/env bash
# Build a UEFI-only El Torito ISO that boots canboot-x86_64-uefi.efi.
# The ESP image embedded as the El Torito boot image contains
# /EFI/BOOT/BOOTX64.EFI, which UEFI firmwares look for automatically.

set -euo pipefail

EFI_BIN="${1:-build/canboot-x86_64-uefi.efi}"
OUT="${2:-build/canboot-x86_64-uefi.iso}"

if [ ! -f "$EFI_BIN" ]; then
    echo "error: EFI binary not found at $EFI_BIN" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# Size the FAT image generously above the .efi. Use FAT32 with at least
# 64 MiB so cluster counts comfortably exceed the FAT32 minimum and OVMF
# accepts it as an ESP.
EFI_BYTES="$(stat -c '%s' "$EFI_BIN")"
ESP_MB="$(( (EFI_BYTES / 1024 / 1024) + 64 ))"

ISO_ROOT="$WORKDIR/iso"
mkdir -p "$ISO_ROOT"

ESP_IMG="$ISO_ROOT/efi.img"
truncate -s "${ESP_MB}M" "$ESP_IMG"
mkfs.vfat -F 32 -n CANBOOTEFI "$ESP_IMG" >/dev/null
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI

# Also place the .efi in the ISO9660 tree so it can be inspected from a
# mounted image; not required for boot.
mkdir -p "$ISO_ROOT/EFI/BOOT"
cp "$EFI_BIN" "$ISO_ROOT/EFI/BOOT/BOOTX64.EFI"

# Embed /init.cdo at the ISO root so the disk selftest can find
# it via ISO9660 when no attached FAT32 disk is present.
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INIT_CDO="$ROOT_DIR/initramfs/init.cdo"
if [ -f "$INIT_CDO" ]; then
    cp "$INIT_CDO" "$ISO_ROOT/init.cdo"
fi
PROBE_PNG="$ROOT_DIR/initramfs/probe.png"
if [ -f "$PROBE_PNG" ]; then
    cp "$PROBE_PNG" "$ISO_ROOT/probe.png"
fi

xorriso -as mkisofs \
    -V "CANBOOT" \
    -no-emul-boot \
    -e efi.img \
    -isohybrid-gpt-basdat \
    -o "$OUT" \
    "$ISO_ROOT"

echo "wrote $OUT"
