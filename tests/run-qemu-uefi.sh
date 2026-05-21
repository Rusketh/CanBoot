#!/usr/bin/env bash
# Boot the UEFI ISO under QEMU + OVMF and assert "ok" appears on COM1.
# OVMF_VARS is copied to a writable temp file because OVMF expects to be
# able to mutate it (boot variables) even when no settings change.

set -euo pipefail

ISO="${1:-build/canboot-x86_64-uefi.iso}"
LOG="${LOG:-build/qemu-uefi.log}"
TIMEOUT="${TIMEOUT:-90}"

if [ ! -f "$ISO" ]; then
    echo "error: iso not found at $ISO" >&2
    exit 1
fi

OVMF_CODE=""
for c in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF.fd \
    /usr/share/qemu/OVMF.fd
do
    if [ -f "$c" ]; then OVMF_CODE="$c"; break; fi
done

if [ -z "$OVMF_CODE" ]; then
    echo "error: OVMF firmware image not found (looked in /usr/share/OVMF and /usr/share/ovmf)" >&2
    exit 1
fi

OVMF_VARS_SRC=""
for v in \
    /usr/share/OVMF/OVMF_VARS_4M.fd \
    /usr/share/OVMF/OVMF_VARS.fd
do
    if [ -f "$v" ]; then OVMF_VARS_SRC="$v"; break; fi
done

WORK="$(mktemp -d)"
mkdir -p "$(dirname "$LOG")"
: > "$LOG"

QEMU_ARGS=(
    -machine q35
    -cdrom "$ISO"
    -serial "file:$LOG"
    -display none
    -no-reboot
    -m 256M
    -monitor none
)

if [ -n "$OVMF_VARS_SRC" ]; then
    cp "$OVMF_VARS_SRC" "$WORK/vars.fd"
    QEMU_ARGS+=(
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,file=$WORK/vars.fd"
    )
else
    QEMU_ARGS+=( -bios "$OVMF_CODE" )
fi

qemu-system-x86_64 "${QEMU_ARGS[@]}" >/dev/null 2>&1 &
QEMU_PID=$!

cleanup() {
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
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
    tr -d '\r' < "$LOG" | sed 's/^/  | /' >&2
else
    echo "  | (empty)" >&2
fi
exit 1
