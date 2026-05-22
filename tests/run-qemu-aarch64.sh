#!/usr/bin/env bash
# Boot the aarch64 raw kernel image via QEMU virt's -kernel path and
# assert the milestone-3 chain ('kmain reached' -> boot_info dump ->
# fdt-derived mmap -> handshake -> ok) appears on PL011 serial within
# TIMEOUT seconds. The kernel must be the .bin (raw image carrying the
# arm64 boot header) so QEMU loads us as a Linux-flavoured kernel and
# passes the FDT phys addr in x0.

set -euo pipefail

ELF="${1:-build-aarch64/canboot-aarch64.bin}"
LOG="${LOG:-build-aarch64/qemu-aarch64.log}"
TIMEOUT="${TIMEOUT:-60}"

if [ ! -f "$ELF" ]; then
    echo "error: kernel ELF not found at $ELF" >&2
    exit 1
fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

QEMU_STDERR="${LOG%.log}.stderr.log"
: > "$QEMU_STDERR"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "error: qemu-system-aarch64 not on PATH" >&2
    echo "PATH=$PATH" >&2
    command -v qemu-system-arm >&2 || true
    exit 1
fi

qemu-system-aarch64 --version >&2 || true

qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a72 \
    -m 256M \
    -nodefaults \
    -net none \
    -display none \
    -no-reboot \
    -kernel "$ELF" \
    -serial "file:$LOG" \
    -monitor none \
    >/dev/null 2>"$QEMU_STDERR" &
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
        check 'canboot: boot_info v1 source=direct-kernel'
        check 'canboot: mmap entries='
        check 'canboot: platform-tables='
        check 'canboot: handshake confirmed (aarch64 milestone-3)'
        check 'milestone 5: starting self-test'
        check 'milestone 5: self-test ok'
        check 'canboot: aarch64 hello world boot complete'

        # FDT walker must have found at least one usable mmap entry
        # (QEMU virt always exposes the main RAM region).
        if ! grep -qE 'canboot:   \[0\] base=0x[0-9a-f]+ len=0x[0-9a-f]+ type=usable' <<<"$stripped"; then
            echo "smoke test FAILED: fdt walker produced no usable mmap entry" >&2
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
    tr -d '\r' < "$LOG" | sed 's/^/  | /' >&2
else
    echo "  | (empty)" >&2
fi
echo "QEMU stderr:" >&2
if [ -s "$QEMU_STDERR" ]; then
    sed 's/^/  ! /' < "$QEMU_STDERR" >&2
else
    echo "  ! (empty)" >&2
fi
if kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "QEMU still running (pid $QEMU_PID)" >&2
else
    echo "QEMU exited before timeout" >&2
fi
exit 1
