#!/usr/bin/env bash
# Boot the aarch64 kernel ELF directly via QEMU virt's -kernel path
# (no UEFI/ISO yet on aarch64) and assert the milestone-1-equivalent
# 'ok' marker appears on PL011 serial within TIMEOUT seconds.

set -euo pipefail

ELF="${1:-build-aarch64/canboot-aarch64.elf}"
LOG="${LOG:-build-aarch64/qemu-aarch64.log}"
TIMEOUT="${TIMEOUT:-60}"

if [ ! -f "$ELF" ]; then
    echo "error: kernel ELF not found at $ELF" >&2
    exit 1
fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a72 \
    -m 256M \
    -display none \
    -no-reboot \
    -kernel "$ELF" \
    -serial "file:$LOG" \
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

        check() {
            local needle="$1"
            if ! grep -q -- "$needle" <<<"$stripped"; then
                echo "smoke test FAILED: missing '$needle'" >&2
                echo "$stripped" | sed 's/^/  | /' >&2
                exit 1
            fi
        }
        check 'canboot: kmain reached (aarch64)'
        check 'canboot: handshake confirmed (aarch64 milestone-1)'
        check 'canboot: aarch64 hello world boot complete'

        echo "smoke test passed; serial log:"
        echo "$stripped" | sed 's/^/  | /'
        exit 0
    fi
    sleep 1
done

echo "smoke test FAILED after ${TIMEOUT}s; serial log so far:" >&2
if [ -s "$LOG" ]; then
    tr -d '\r' < "$LOG" | sed 's/^/  | /' >&2
else
    echo "  | (empty)" >&2
fi
exit 1
