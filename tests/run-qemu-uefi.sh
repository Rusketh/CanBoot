#!/usr/bin/env bash
# Boot the UEFI ISO under QEMU + OVMF with PS/2, virtio-keyboard, and
# virtio-net. Inject keystrokes via HMP, run sidecar UDP echo + HTTP
# servers reachable via the SLIRP gateway, and assert the full
# milestone 1-6 chain before "ok".

set -euo pipefail

ISO="${1:-build/canboot-x86_64-uefi.iso}"
LOG="${LOG:-build/qemu-uefi.log}"
TIMEOUT="${TIMEOUT:-150}"

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

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
MON_SOCK="$WORK/mon.sock"
UDP_PORT="${UDP_PORT:-7777}"
HTTP_PORT="${HTTP_PORT:-8080}"

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

python3 "$ROOT/tests/sidecars/udp_echo.py"   127.0.0.1 "$UDP_PORT"  >"$WORK/udp.log"  2>&1 &
UDP_PID=$!
python3 "$ROOT/tests/sidecars/http_hello.py" 127.0.0.1 "$HTTP_PORT" >"$WORK/http.log" 2>&1 &
HTTP_PID=$!
sleep 0.4

QEMU_ARGS=(
    -machine q35
    -cdrom "$ISO"
    -device virtio-keyboard-pci
    -netdev user,id=n0
    -device virtio-net-pci,netdev=n0
    -serial "file:$LOG"
    -monitor "unix:$MON_SOCK,server,nowait"
    -display none
    -no-reboot
    -m 256M
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

(
    for _ in $(seq 1 30); do
        [ -S "$MON_SOCK" ] && break
        sleep 0.2
    done
    for _ in $(seq 1 100); do
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
    for pid in "$INJECTOR_PID" "$QEMU_PID" "$UDP_PID" "$HTTP_PID"; do
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
        check 'canboot: framebuffer painted\|canboot: fb = '
        check 'canboot: ps/2 input ready'
        check 'canboot: virtio-input ready'
        check 'canboot: rx '
        check 'milestone 5: self-test ok'
        check 'milestone 6: dhcp lease'
        check 'milestone 6: udp echo ok'
        check 'milestone 6: http get ok'
        check 'milestone 6: net test ok'

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
