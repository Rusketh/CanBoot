#!/usr/bin/env bash
# Boot the aarch64 UEFI PE/COFF via qemu-system-aarch64 + AAVMF firmware,
# and assert the milestone-2-equivalent markers appear on PL011 serial
# within TIMEOUT seconds.

set -euo pipefail

IMG="${1:-build-aarch64/canboot-aarch64-uefi.img}"
LOG="${LOG:-build-aarch64/qemu-aarch64-uefi.log}"
TIMEOUT="${TIMEOUT:-90}"

AAVMF_CODE="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}"
AAVMF_VARS_SRC="${AAVMF_VARS_SRC:-/usr/share/AAVMF/AAVMF_VARS.fd}"

if [ ! -f "$IMG" ]; then
    echo "error: ESP image not found at $IMG" >&2
    exit 1
fi
if [ ! -f "$AAVMF_CODE" ]; then
    echo "error: AAVMF_CODE.fd not found at $AAVMF_CODE" >&2
    echo "       install qemu-efi-aarch64 (Debian/Ubuntu)" >&2
    exit 1
fi
if [ ! -f "$AAVMF_VARS_SRC" ]; then
    echo "error: AAVMF_VARS.fd not found at $AAVMF_VARS_SRC" >&2
    exit 1
fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

QEMU_STDERR="${LOG%.log}.stderr.log"
: > "$QEMU_STDERR"

# QEMU mmaps the vars file r/w; copy it so we don't mutate the package
# install and so concurrent CI jobs don't fight over it.
AAVMF_VARS="$(dirname "$LOG")/AAVMF_VARS.fd"
cp "$AAVMF_VARS_SRC" "$AAVMF_VARS"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "error: qemu-system-aarch64 not on PATH" >&2
    exit 1
fi
qemu-system-aarch64 --version >&2 || true

qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -nodefaults \
    -net none \
    -display none \
    -no-reboot \
    -drive if=pflash,format=raw,readonly=on,file="$AAVMF_CODE" \
    -drive if=pflash,format=raw,file="$AAVMF_VARS" \
    -drive if=none,id=hd0,format=raw,file="$IMG" \
    -device virtio-blk-pci,drive=hd0,bootindex=0 \
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
        check 'canboot: uefi entry reached (aarch64)'
        check 'canboot: calling ExitBootServices (aarch64)'
        check 'canboot: kmain reached (aarch64)'
        check 'canboot: boot_info v1 source=uefi'
        check 'canboot: mmap entries='
        check 'canboot: platform-tables='
        check 'canboot: handshake confirmed (aarch64 milestone-3)'

        # Framebuffer path: Debian/Ubuntu AAVMF ships a stripped firmware
        # build with no GOP-producing drivers, so the loader's
        # LocateProtocol(GOP) returns NOT_FOUND and the kmain reports
        # "fb = none". We still verify the kmain's fb branch ran at all
        # (one of the two messages must appear), and prefer the painted
        # path when GOP is available.
        if ! grep -qE 'canboot: (fb rgb addr=|fb = none)' <<<"$stripped"; then
            echo "smoke test FAILED: kmain never entered the fb branch" >&2
            echo "$stripped" | sed 's/^/  | /' >&2
            exit 1
        fi
        if grep -q 'canboot: framebuffer painted' <<<"$stripped"; then
            echo "(framebuffer was provided by AAVMF and painted)" >&2
        else
            echo "(AAVMF reported no GOP; framebuffer path validated only via 'fb = none' branch)" >&2
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
