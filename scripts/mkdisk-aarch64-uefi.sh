#!/usr/bin/env bash
# Build a raw FAT32 ESP image containing /EFI/BOOT/BOOTAA64.EFI so AAVMF
# can boot canboot under qemu-system-aarch64 -bios AAVMF_CODE.fd.

set -euo pipefail

EFI_BIN="${1:-build-aarch64/canboot-aarch64-uefi.efi}"
OUT="${2:-build-aarch64/canboot-aarch64-uefi.img}"

if [ ! -f "$EFI_BIN" ]; then
    echo "error: EFI binary not found at $EFI_BIN" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

EFI_BYTES="$(stat -c '%s' "$EFI_BIN")"
ESP_MB="$(( (EFI_BYTES / 1024 / 1024) + 64 ))"

truncate -s "${ESP_MB}M" "$OUT"
mkfs.vfat -F 32 -n CANBOOTEFI "$OUT" >/dev/null
mmd -i "$OUT" ::/EFI
mmd -i "$OUT" ::/EFI/BOOT
mcopy -i "$OUT" "$EFI_BIN" ::/EFI/BOOT/BOOTAA64.EFI

INIT_CDO="$(cd "$(dirname "$0")/.." && pwd)/initramfs/init.cdo"
if [ -f "$INIT_CDO" ]; then
    mcopy -i "$OUT" "$INIT_CDO" ::/init.cdo
fi

echo "wrote $OUT"
