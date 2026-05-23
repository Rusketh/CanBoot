#!/usr/bin/env bash
# Generate a small NTFS test image with a known file inside, so the
# smoke test can exercise the libntfs-3g read path. Output: a raw
# image with one NTFS partition and /probe.txt containing a known
# marker.
#
# This script avoids FUSE entirely (no `ntfs-3g <img> <mnt>` mount).
# CI runners (GitHub Actions / sandboxed containers) frequently lack
# /dev/fuse, which would silently break the smoke test when the test
# image fails to populate. ntfscp from the ntfs-3g package operates
# directly on the image without mounting, so the same machinery works
# regardless of FUSE availability.

set -euo pipefail

OUT="${1:-build/canboot-ntfs-test.img}"
SIZE_MB="${2:-16}"

if ! command -v mkfs.ntfs >/dev/null 2>&1; then
    echo "error: mkfs.ntfs not on PATH (apt install ntfs-3g)" >&2
    exit 1
fi
if ! command -v ntfscp >/dev/null 2>&1; then
    echo "error: ntfscp not on PATH (apt install ntfs-3g)" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
truncate -s "${SIZE_MB}M" "$OUT"

# Format with NTFS. --fast skips zero-fill; --force lets us mkfs onto
# a plain file. Label = CANNTFS so the smoke test can match it.
mkfs.ntfs --fast --force --label CANNTFS "$OUT" >/dev/null 2>&1

# Offline file injection via ntfscp (same ntfs-3g package as mkfs.ntfs,
# but no FUSE needed). -f overrides the "is this a real block device"
# check; the source is read from a regular file on the host.
TMP_MARK="$(mktemp)"
trap 'rm -f "$TMP_MARK"' EXIT
printf 'canboot-ntfs-marker-2026\n' > "$TMP_MARK"
ntfscp -f "$OUT" "$TMP_MARK" probe.txt >/dev/null 2>&1

echo "wrote $OUT (NTFS, $SIZE_MB MiB, /probe.txt with marker)"
