#!/usr/bin/env bash
# Generate a small NTFS test image with a known file inside, so the
# smoke test can exercise the libntfs-3g read path. Output: a raw
# image with one NTFS partition and /ntfs-probe.txt containing a
# known marker.

set -euo pipefail

OUT="${1:-build/canboot-ntfs-test.img}"
SIZE_MB="${2:-16}"

if ! command -v mkfs.ntfs >/dev/null 2>&1; then
    echo "error: mkfs.ntfs not on PATH (apt install ntfs-3g)" >&2
    exit 1
fi
if ! command -v ntfs-3g >/dev/null 2>&1; then
    echo "error: ntfs-3g not on PATH" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
TMPDIR_M="$(mktemp -d)"
trap 'fusermount -u "$TMPDIR_M" 2>/dev/null || umount "$TMPDIR_M" 2>/dev/null || true; rm -rf "$TMPDIR_M"' EXIT

truncate -s "${SIZE_MB}M" "$OUT"

# Format with NTFS. --fast skips zero-fill; --force lets us mkfs onto
# a plain file. Label = CANNTFS so the smoke test can match it.
mkfs.ntfs --fast --force --label CANNTFS "$OUT" >/dev/null 2>&1

# Mount via ntfs-3g over the file (loopback) and drop in a known file.
mkdir -p "$TMPDIR_M"
ntfs-3g "$OUT" "$TMPDIR_M" -o loop,noatime
printf 'canboot-ntfs-marker-2026\n' > "$TMPDIR_M/probe.txt"
sync
umount "$TMPDIR_M" 2>/dev/null || fusermount -u "$TMPDIR_M"

echo "wrote $OUT (NTFS, $SIZE_MB MiB, /probe.txt with marker)"
