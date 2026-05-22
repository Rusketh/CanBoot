#!/usr/bin/env bash
# Build a BIOS-bootable El Torito ISO that GRUB2 will hand off to canboot
# via Multiboot2. UEFI/hybrid variants land in later milestones.

set -euo pipefail

KERNEL="${1:-build/canboot-x86_64.elf}"
OUT="${2:-build/canboot-x86_64-bios.iso}"

if [ ! -f "$KERNEL" ]; then
    echo "error: kernel ELF not found at $KERNEL" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

mkdir -p "$WORKDIR/boot/grub"
cp "$KERNEL" "$WORKDIR/boot/canboot.elf"

# Embed /init.cdo at the ISO root so the milestone 8 disk test can find
# it via ISO9660 when no attached FAT32 disk is present.
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INIT_CDO="$ROOT_DIR/initramfs/init.cdo"
if [ -f "$INIT_CDO" ]; then
    cp "$INIT_CDO" "$WORKDIR/init.cdo"
fi
PROBE_PNG="$ROOT_DIR/initramfs/probe.png"
if [ -f "$PROBE_PNG" ]; then
    cp "$PROBE_PNG" "$WORKDIR/probe.png"
fi

cat > "$WORKDIR/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

menuentry "canboot" {
    multiboot2 /boot/canboot.elf
    boot
}
EOF

grub-mkrescue \
    --modules="multiboot2 normal" \
    -o "$OUT" \
    "$WORKDIR" \
    >/dev/null 2>&1 || grub-mkrescue -o "$OUT" "$WORKDIR"

echo "wrote $OUT"
