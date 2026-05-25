#!/usr/bin/env bash
# Build a small FAT32 disk image carrying /init.cdo at the root for the
# disk selftest. Attached to QEMU as virtio-blk.

set -euo pipefail

OUT="${1:-build/canboot-fat32.img}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INIT_CDO="$ROOT/initramfs/init.cdo"
SIZE_MB="${SIZE_MB:-64}"

if [ ! -f "$INIT_CDO" ]; then
    echo "error: $INIT_CDO not found" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

truncate -s "${SIZE_MB}M" "$OUT"
mkfs.vfat -F 32 -n CANBOOT "$OUT" >/dev/null
mcopy -i "$OUT" "$INIT_CDO" ::/init.cdo

PROBE_PNG="$ROOT/initramfs/probe.png"
if [ -f "$PROBE_PNG" ]; then
    mcopy -i "$OUT" "$PROBE_PNG" ::/probe.png
fi

# Derma GUI library + demo so booted scripts can include("/derma.cdo").
for f in derma.cdo derma_demo.cdo; do
    if [ -f "$ROOT/initramfs/$f" ]; then
        mcopy -i "$OUT" "$ROOT/initramfs/$f" "::/$f"
    fi
done

echo "wrote $OUT"
