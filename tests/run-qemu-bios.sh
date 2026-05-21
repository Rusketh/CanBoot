#!/usr/bin/env bash
# Boot the BIOS ISO in QEMU and assert that the kernel prints "ok" on COM1
# within TIMEOUT seconds. This is the canonical milestone smoke test.

set -euo pipefail

ISO="${1:-build/canboot-x86_64-bios.iso}"
LOG="${LOG:-build/qemu-bios.log}"
TIMEOUT="${TIMEOUT:-60}"

if [ ! -f "$ISO" ]; then
    echo "error: iso not found at $ISO" >&2
    exit 1
fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

qemu-system-x86_64 \
    -cdrom "$ISO" \
    -serial "file:$LOG" \
    -display none \
    -no-reboot \
    -m 128M \
    -nographic \
    -monitor none \
    >/dev/null 2>&1 &
QEMU_PID=$!

cleanup() {
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if tr -d '\r' < "$LOG" 2>/dev/null | grep -q '^ok$'; then
        stripped="$(tr -d '\r' < "$LOG")"
        if ! grep -q 'canboot: framebuffer painted\|canboot: fb = ' <<<"$stripped"; then
            echo "smoke test FAILED: 'ok' seen but unified boot_info path didn't run" >&2
            echo "$stripped" | sed 's/^/  | /' >&2
            exit 1
        fi
        echo "smoke test passed; serial log:"
        echo "$stripped" | sed 's/^/  | /'
        exit 0
    fi
    sleep 1
done

echo "smoke test FAILED after ${TIMEOUT}s; serial log so far:" >&2
if [ -s "$LOG" ]; then
    sed 's/^/  | /' "$LOG" >&2
else
    echo "  | (empty)" >&2
fi
exit 1
