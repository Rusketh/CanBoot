#!/usr/bin/env bash
# Generate a small ext4 test image with a known file inside, so the
# smoke test can exercise the lwext4 read path. Output: a raw image
# formatted ext4 with /probe.txt containing a known marker.

set -euo pipefail

OUT="${1:-build/canboot-ext4-test.img}"
SIZE_MB="${2:-32}"

if ! command -v mkfs.ext4 >/dev/null 2>&1; then
    echo "error: mkfs.ext4 not on PATH (apt install e2fsprogs)" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
truncate -s "${SIZE_MB}M" "$OUT"

# Format with ext4, label CANEXT4 so smoke test can match. -F forces
# mkfs onto a regular file; -E nodiscard skips the TRIM pass (no-op
# on a regular file and noisy on stderr).
mkfs.ext4 -F -L CANEXT4 -E nodiscard "$OUT" >/dev/null 2>&1

# Drop a marker file inside using debugfs (no loopback root needed).
# debugfs's `write SRC NAME` takes a source path on the host; pipe-
# from-stdin doesn't work reliably.
TMP_MARK="$(mktemp)"
trap 'rm -f "$TMP_MARK"' EXIT
printf 'canboot-ext4-marker-2026\n' > "$TMP_MARK"
debugfs -w -R "write $TMP_MARK probe.txt" "$OUT" >/dev/null 2>&1

echo "wrote $OUT (ext4, $SIZE_MB MiB, /probe.txt with marker)"
