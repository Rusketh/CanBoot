#!/usr/bin/env bash
# Boot the BIOS ISO in QEMU with a PS/2 keyboard, virtio-keyboard, and
# virtio-net. Inject keystrokes via the HMP monitor once kmain reaches
# its polling loop, run sidecar UDP echo + HTTP servers reachable via
# the SLIRP host gateway (10.0.2.2), and assert the milestone 1-6
# checkpoints before "ok".

set -euo pipefail

ISO="${1:-build/canboot-x86_64-bios.iso}"
LOG="${LOG:-build/qemu-bios.log}"
TIMEOUT="${TIMEOUT:-180}"

if [ ! -f "$ISO" ]; then
    echo "error: iso not found at $ISO" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
MON_SOCK="$WORK/mon.sock"
UDP_PORT="${UDP_PORT:-7777}"
HTTP_PORT="${HTTP_PORT:-8080}"

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

HTTPS_PORT="${HTTPS_PORT:-8443}"

python3 "$ROOT/tests/sidecars/udp_echo.py"    127.0.0.1 "$UDP_PORT"   >"$WORK/udp.log"   2>&1 &
UDP_PID=$!
python3 "$ROOT/tests/sidecars/http_hello.py"  127.0.0.1 "$HTTP_PORT"  >"$WORK/http.log"  2>&1 &
HTTP_PID=$!
python3 "$ROOT/tests/sidecars/https_secure.py" 127.0.0.1 "$HTTPS_PORT" >"$WORK/https.log" 2>&1 &
HTTPS_PID=$!
sleep 0.5

qemu-system-x86_64 \
    -machine q35 \
    -cdrom "$ISO" \
    -device virtio-keyboard-pci \
    -netdev user,id=n0 \
    -device virtio-net-pci,netdev=n0 \
    -serial "file:$LOG" \
    -monitor "unix:$MON_SOCK,server,nowait" \
    -display none \
    -no-reboot \
    -m 256M \
    -nographic \
    >/dev/null 2>&1 &
QEMU_PID=$!

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
    for pid in "$INJECTOR_PID" "$QEMU_PID" "$UDP_PID" "$HTTP_PID" "$HTTPS_PID"; do
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
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
        check 'milestone 6: dhcp lease'
        check 'milestone 6: udp echo ok'
        check 'milestone 6: http get ok'
        check 'milestone 6: net test ok'
        check 'milestone 7: handshake ok'
        check 'milestone 7: https get ok'
        check 'milestone 7: session resumption ok'
        check 'milestone 7: tls test ok'

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
