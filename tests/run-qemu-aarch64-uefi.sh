#!/usr/bin/env bash
# Boot the aarch64 UEFI PE/COFF via qemu-system-aarch64 + AAVMF firmware,
# and assert the milestone-2-equivalent markers appear on PL011 serial
# within TIMEOUT seconds.

set -euo pipefail

IMG="${1:-build-aarch64/canboot-aarch64-uefi.img}"
LOG="${LOG:-build-aarch64/qemu-aarch64-uefi.log}"
TIMEOUT="${TIMEOUT:-360}"

AAVMF_CODE="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}"
AAVMF_VARS_SRC="${AAVMF_VARS_SRC:-/usr/share/AAVMF/AAVMF_VARS.fd}"
MON_SOCK="${MON_SOCK:-$(dirname "$LOG")/mon.sock}"
UDP_PORT="${UDP_PORT:-7777}"
HTTP_PORT="${HTTP_PORT:-8080}"
HTTPS_PORT="${HTTPS_PORT:-8443}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

python3 "$ROOT/tests/sidecars/udp_echo.py"    127.0.0.1 "$UDP_PORT"   >"$(dirname "$LOG")/udp.log"   2>&1 &
UDP_PID=$!
python3 "$ROOT/tests/sidecars/http_hello.py"  127.0.0.1 "$HTTP_PORT"  >"$(dirname "$LOG")/http.log"  2>&1 &
HTTP_PID=$!
python3 "$ROOT/tests/sidecars/https_secure.py" 127.0.0.1 "$HTTPS_PORT" >"$(dirname "$LOG")/https.log" 2>&1 &
HTTPS_PID=$!
sleep 0.3

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
    -display none \
    -no-reboot \
    -drive if=pflash,format=raw,readonly=on,file="$AAVMF_CODE" \
    -drive if=pflash,format=raw,file="$AAVMF_VARS" \
    -drive if=none,id=hd0,format=raw,file="$IMG" \
    -device virtio-blk-pci,drive=hd0,bootindex=0 \
    -device virtio-keyboard-pci \
    -device virtio-gpu-pci \
    -netdev user,id=n0 \
    -device virtio-net-pci,netdev=n0,romfile= \
    -serial "file:$LOG" \
    -monitor "unix:$MON_SOCK,server,nowait" \
    >/dev/null 2>"$QEMU_STDERR" &
QEMU_PID=$!

# Background injector: two waves.
#  - m4: send "a b ret" once "canboot: input loop start" appears so the
#    raw HAL input pump receives them.
#  - m12: send "x y z" once "cando input poll begin" appears so cando's
#    input.waitKey() inside /init.cdo receives them via the same pump.
(
    for _ in $(seq 1 30); do
        [ -S "$MON_SOCK" ] && break
        sleep 0.2
    done
    for _ in $(seq 1 200); do
        if grep -q 'canboot: input loop start' "$LOG" 2>/dev/null; then
            break
        fi
        sleep 0.1
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
    for _ in $(seq 1 600); do
        if grep -q 'cando input poll begin' "$LOG" 2>/dev/null; then
            break
        fi
        sleep 0.1
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
    for pid in "$INJECTOR_PID" "$UDP_PID" "$HTTP_PID" "$HTTPS_PID"; do
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
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
        check 'canboot: pci devs='
        check 'canboot: virtio-input present'
        check 'canboot: input loop start'
        check 'canboot: input loop done events='
        if ! grep -qE "canboot: rx code=0x[0-9a-f]+ ascii='a'" <<<"$stripped"; then
            echo "smoke test FAILED: virtio-input did not echo injected 'a'" >&2
            echo "$stripped" | sed 's/^/  | /' >&2
            exit 1
        fi
        check 'milestone 5: starting self-test'
        check 'milestone 5: self-test ok'
        check 'milestone 6: udp echo ok'
        check 'milestone 6: http get ok'
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
        check 'milestone 10: cando_dostring ok'
        check 'milestone 10: init.cdo executed ok'
        check 'canboot: virtio-gpu fb '
        check 'milestone 11: display lib registered'
        check 'milestone 11: display test ok'
        check 'milestone 12: input lib registered'
        check 'milestone 13: system libs registered'
        check 'cando time.ms ='
        check 'cando file.exists(init.cdo) = true'
        check 'cando file.read = hello-from-cando'
        check 'cando net.udpEcho = cando-udp-probe'
        check 'cando net.httpGet = canboot-hello'
        check 'cando tls.httpsGet = canboot-secure'
        check 'cando sys libs end'
        check 'milestone 14: crypto libs registered'
        check 'cando hex.encode(canboot) = 63616e626f6f74'
        check 'cando hex.decode(63616e626f6f74) = canboot'
        check 'cando base64.encode(canboot) = Y2FuYm9vdA=='
        check 'cando base64.decode(Y2FuYm9vdA==) = canboot'
        check 'cando crypto.sha256Hex(empty) = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
        check 'cando crypto.hmacSha256Hex(k, m) = f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8'
        check 'cando crypto libs end'
        check 'milestone 15: env+log libs registered'
        check 'cando env.source = uefi'
        check 'cando env.fbFormat = rgb'
        check 'INFO  info-level message from cando'
        check 'WARN  warn-level message from cando'
        check 'ERROR error-level message from cando'
        check 'INIT.CDO'
        check 'cando intro libs end'

        # log.setLevel("info") must suppress the trailing debug log:
        if grep -q 'THIS-SHOULD-BE-SUPPRESSED' <<<"$stripped"; then
            echo "smoke test FAILED: log.setLevel(info) did not suppress debug message" >&2
            exit 1
        fi

        check 'milestone 16: extension libs registered'
        check 'cando url.scheme = https'
        check 'cando url.host = 10.0.2.2'
        check 'cando url.port = 8443'
        check 'cando url.path = /health'
        check 'cando http.get = canboot-hello'
        check 'cando https.get = canboot-secure'
        check 'cando disk.name(0) = vblk0'
        check 'cando disk.blockSize(0) = 512'
        check 'cando fmt.sprintf = hex=1234 dec=42 str=hi'
        check 'cando ext libs end'
        check 'milestone 17: partition+fs libs registered'
        check 'cando partition.scheme(0) = none'
        check 'cando fs.detect(0,0) = unknown'
        check 'cando part libs end'
        check 'cando input poll begin'
        check 'cando input poll end'

        # m12 second-wave injection: cando's input.waitKey() inside
        # /init.cdo should have received x/y/z. The script prints
        # "cando got key1: <ascii>" for each successful read; a null
        # means waitKey timed out before the injector fired.
        if ! grep -q "cando got key1: 120" <<<"$stripped"; then
            echo "smoke test FAILED: cando input.waitKey did not receive 'x' (120)" >&2
            echo "$stripped" | sed 's/^/  | /' >&2
            exit 1
        fi

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
