#!/usr/bin/env bash
# Build a GPT-partitioned aarch64 UEFI test image with TWO partitions:
#   1. EFI System (FAT32, 32 MiB) - holds /EFI/BOOT/BOOTAA64.EFI and
#      our /init.cdo. AAVMF discovers + boots BOOTAA64.EFI from here.
#   2. NTFS data (32 MiB) - carries /probe.txt with a known marker
#      string. The smoke test exercises libntfs-3g read against it.
#
# Together they let init.cdo call partition.list(0), fs.detect(0,1) ==
# "ntfs", fs.read(0,1,"probe.txt") == "canboot-ntfs-marker-2026", all
# from the same boot disk.

set -euo pipefail

EFI_BIN="${1:-build-aarch64/canboot-aarch64-uefi.efi}"
OUT="${2:-build-aarch64/canboot-aarch64-uefi-multi.img}"

if [ ! -f "$EFI_BIN" ]; then
    echo "error: EFI binary not found at $EFI_BIN" >&2
    exit 1
fi
for t in sgdisk mkfs.vfat mkfs.ntfs ntfs-3g mtools; do
    command -v "$t" >/dev/null 2>&1 || command -v "$(basename "$t")" >/dev/null 2>&1 || {
        if [ "$t" != "mtools" ]; then
            echo "error: $t not on PATH" >&2; exit 1
        fi
    }
done

ESP_MB=32
NTFS_MB=32
TOTAL_MB=$((ESP_MB + NTFS_MB + 2))   # +2 MiB for GPT headers + slack

mkdir -p "$(dirname "$OUT")"
truncate -s "${TOTAL_MB}M" "$OUT"

# Lay down GPT with the two partitions.
sgdisk --zap-all "$OUT" >/dev/null
sgdisk \
    --new=1:2048:+${ESP_MB}M --typecode=1:ef00 --change-name=1:CANBOOTESP \
    --new=2:0:+${NTFS_MB}M  --typecode=2:0700 --change-name=2:CANNTFS \
    "$OUT" >/dev/null

# Slice the partitions out so we can format each independently with
# fallocate-friendly offsets via loop devices.
ESP_START=$((2048 * 512))
ESP_BYTES=$((ESP_MB * 1024 * 1024))
NTFS_START=$((ESP_START + ESP_BYTES))
NTFS_BYTES=$((NTFS_MB * 1024 * 1024))

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- Build the ESP image ---
ESP_IMG="$WORK/esp.img"
truncate -s "${ESP_MB}M" "$ESP_IMG"
mkfs.vfat -F 32 -n CANBOOTEFI "$ESP_IMG" >/dev/null
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTAA64.EFI
RAMFS_DIR="$(cd "$(dirname "$0")/../.." && pwd)/initramfs"
INIT_CDO="$RAMFS_DIR/init.cdo"
if [ -f "$INIT_CDO" ]; then
    mcopy -i "$ESP_IMG" "$INIT_CDO" ::/init.cdo
fi

# --- Build the NTFS image ---
NTFS_IMG="$WORK/ntfs.img"
truncate -s "${NTFS_MB}M" "$NTFS_IMG"
mkfs.ntfs --fast --force --label CANNTFS "$NTFS_IMG" >/dev/null 2>&1

MNT="$WORK/m"
mkdir -p "$MNT"
ntfs-3g "$NTFS_IMG" "$MNT" -o loop,noatime
printf 'canboot-ntfs-marker-2026\n' > "$MNT/probe.txt"
mkdir -p "$MNT/dir1"
printf 'canboot-subdir-content\n' > "$MNT/dir1/nested.txt"
sync
fusermount -u "$MNT" 2>/dev/null || umount "$MNT"

# --- Splice both into the GPT-partitioned image ---
dd if="$ESP_IMG"  of="$OUT" bs=1M seek=1                        conv=notrunc status=none
dd if="$NTFS_IMG" of="$OUT" bs=1M seek=$((1 + ESP_MB))           conv=notrunc status=none

echo "wrote $OUT (GPT, ESP=$ESP_MB MiB + NTFS=$NTFS_MB MiB)"
