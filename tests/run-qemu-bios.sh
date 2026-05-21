#!/usr/bin/env bash
# Boot the BIOS ISO in QEMU with both a PS/2 keyboard (default) and a
# virtio-keyboard, inject a short keystroke sequence via the HMP monitor
# once kmain has reached its polling input loop, and assert that the
# unified boot-info path, both input drivers, and at least one received
# keystroke all show up on the serial log before "ok".

set -euo pipefail

ISO="${1:-build/canboot-x86_64-bios.iso}"
LOG="${LOG:-build/qemu-bios.log}"
TIMEOUT="${TIMEOUT:-90}"

if [ ! -f "$ISO" ]; then
    echo "error: iso not found at $ISO" >&2
    exit 1
fi

WORK="$(mktemp -d)"
MON_SOCK="$WORK/mon.sock"

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

qemu-system-x86_64 \
    -machine q35 \
    -cdrom "$ISO" \
    -device virtio-keyboard-pci \
    -serial "file:$LOG" \
    -monitor "unix:$MON_SOCK,server,nowait" \
    -display none \
    -no-reboot \
    -m 256M \
    -nographic \
    >/dev/null 2>&1 &
QEMU_PID=$!

# Background keystroke injector. Waits for the monitor socket and for
# kmain's polling loop to start before connecting, then HMP-sends a
# short sequence and disconnects.
(
    for _ in $(seq 1 30); do
        [ -S "$MON_SOCK" ] && break
        sleep 0.2
    done
    for _ in $(seq 1 60); do
        if grep -q 'canboot: input loop start' "$LOG" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done
    python3 - "$MON_SOCK" <<'PY' 2>/dev/null || true
import socket, sys, time
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    sock.connect(sys.argv[1])
except Exception:
    sys.exit(0)
time.sleep(0.2)
for k in ("a", "b", "ret"):
    sock.sendall(("sendkey " + k + "\n").encode())
    time.sleep(0.3)
sock.close()
PY
) &
INJECTOR_PID=$!

cleanup() {
    if kill -0 "$INJECTOR_PID" 2>/dev/null; then
        kill "$INJECTOR_PID" 2>/dev/null || true
        wait "$INJECTOR_PID" 2>/dev/null || true
    fi
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

        check() {
            local needle="$1"
            if ! grep -q -- "$needle" <<<"$stripped"; then
                echo "smoke test FAILED: missing '$needle'" >&2
                echo "$stripped" | sed 's/^/  | /' >&2
                exit 1
            fi
        }
        check 'canboot: framebuffer painted'
        check 'canboot: ps/2 input ready'
        check 'canboot: virtio-input ready'
        check 'canboot: rx '
        check 'milestone 5: self-test ok'

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
