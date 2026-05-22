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

DISK_IMG="${DISK_IMG:-build/canboot-fat32.img}"
if [ ! -f "$DISK_IMG" ]; then
    "$ROOT/scripts/mkdisk-fat32.sh" "$DISK_IMG" >/dev/null
fi

qemu-system-x86_64 \
    -machine q35 \
    -boot order=dc \
    -cdrom "$ISO" \
    -drive "if=none,id=blk0,file=$DISK_IMG,format=raw" \
    -device virtio-blk-pci,drive=blk0 \
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

    # Second wave for milestone 12: cando's input.waitKey loop.
    for _ in $(seq 1 200); do
        if grep -q 'cando input poll begin' "$LOG" 2>/dev/null; then
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
time.sleep(0.3)
for k in ("x", "y", "z"):
    sock.sendall(("sendkey " + k + "\n").encode())
    time.sleep(0.4)
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

SCREENDUMP_DONE=""
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    # Once init.cdo finishes painting, grab a screendump for the
    # milestone-11 sha256 compare. We do this once, on the first
    # iteration after the paint marker appears.
    if [ -z "$SCREENDUMP_DONE" ] && \
       tr -d '\r' < "$LOG" 2>/dev/null | grep -q 'init.cdo painted display'; then
        echo "smoke: paint marker seen, sending screendump"
        python3 - "$MON_SOCK" "$WORK/screen.ppm" <<'PY' || true
import socket, sys, time, os
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(sys.argv[1])
except Exception as e:
    print("screendump connect failed:", e, file=sys.stderr); sys.exit(0)
s.settimeout(5)
time.sleep(0.3)
# Drain monitor greeting.
try:
    s.recv(4096)
except Exception:
    pass
s.sendall(("screendump " + sys.argv[2] + "\n").encode())
# Wait for the file to materialise and stabilise.
for _ in range(40):
    time.sleep(0.25)
    if os.path.exists(sys.argv[2]) and os.path.getsize(sys.argv[2]) > 4096:
        break
s.close()
PY
        if [ -f "$WORK/screen.ppm" ]; then
            echo "smoke: screendump $(stat -c '%s' "$WORK/screen.ppm") bytes"
        else
            echo "smoke: screendump file NOT created" >&2
        fi
        SCREENDUMP_DONE=1
    fi

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
        check 'milestone 8: init.cdo marker ok'
        check 'milestone 8: disk test ok'
        check 'milestone 9: cando_open ok'
        check 'milestone 9: cando_openlibs ok'
        check 'milestone 9: cando_close ok'
        check 'milestone 9: cando link test ok'
        check 'canboot-cando-runtime-marker'
        check 'milestone 10: cando_dostring ok'
        check 'milestone 10: init.cdo executed ok'
        check 'milestone 11: display lib registered'
        check 'milestone 11: display test ok'
        check 'milestone 12: input lib registered'
        check 'cando input poll begin'
        check 'cando got key1: 120'
        check 'cando got key2: 121'
        check 'cando got key3: 122'
        check 'cando input poll end'

        # Milestone 11 screenshot sha256 compare.
        if [ -f "$WORK/screen.ppm" ]; then
            EXPECTED=$(cat "$ROOT/tests/refs/m11-bios.ppm.sha256" 2>/dev/null | head -1)
            GOT=$(sha256sum "$WORK/screen.ppm" | awk '{print $1}')
            if [ "$EXPECTED" = "$GOT" ]; then
                echo "milestone 11: screendump sha256 matches reference ($GOT)"
            else
                echo "smoke test FAILED: m11 screendump sha256 mismatch" >&2
                echo "  expected: $EXPECTED" >&2
                echo "  got     : $GOT" >&2
                cp "$WORK/screen.ppm" build/m11-bios-actual.ppm 2>/dev/null || true
                exit 1
            fi
        else
            echo "smoke test FAILED: m11 screendump missing" >&2
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
