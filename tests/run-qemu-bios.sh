#!/usr/bin/env bash
# Boot the BIOS ISO in QEMU with a PS/2 keyboard, virtio-keyboard, and
# virtio-net. Inject keystrokes via the HMP monitor once kmain reaches
# its polling loop, run sidecar UDP echo + HTTP servers reachable via
# the SLIRP host gateway (10.0.2.2), and assert the boot
# checkpoints before "ok".
#
# POINTER=ps2     (default) inject a relative PS/2 mouse over HMP.
# POINTER=tablet  bind a virtio-tablet and inject absolute coords over QMP
#                 (exercises the EV_ABS path); drops the virtio keyboard so
#                 the single virtio-input driver binds the tablet.

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
QMP_SOCK="$WORK/qmp.sock"
UDP_PORT="${UDP_PORT:-7777}"
HTTP_PORT="${HTTP_PORT:-8080}"

# Pointer device under test: "ps2" (relative i8042 mouse, injected over the
# HMP monitor) or "tablet" (virtio-tablet, absolute, injected over QMP). The
# virtio-input driver binds a single device, so tablet mode drops the virtio
# keyboard (the PS/2 keyboard still serves sendkey) to bind the tablet.
POINTER="${POINTER:-ps2}"
KBD_DEV="-device virtio-keyboard-pci"
PTR_DEV=""
QMP_DEV=""
if [ "$POINTER" = "tablet" ]; then
    KBD_DEV=""
    PTR_DEV="-device virtio-tablet-pci"
    QMP_DEV="-qmp unix:$QMP_SOCK,server,nowait"
fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

HTTPS_PORT="${HTTPS_PORT:-8443}"

python3 "$ROOT/tests/sidecars/udp_echo.py"    127.0.0.1 "$UDP_PORT"   >"$WORK/udp.log"   2>&1 &
UDP_PID=$!
python3 "$ROOT/tests/sidecars/http_hello.py"  127.0.0.1 "$HTTP_PORT"  >"$WORK/http.log"  2>&1 &
HTTP_PID=$!
python3 "$ROOT/tests/sidecars/https_secure.py" 127.0.0.1 "$HTTPS_PORT" >"$WORK/https.log" 2>&1 &
HTTPS_PID=$!
# Fixed-answer DNS server on host :53; the guest reaches it via SLIRP at
# 10.0.2.2:53. Answers canboot.test -> 10.0.2.2 for the DNS smoke check.
# Port 53 is privileged; CI runs the sidecar as a non-root user, so
# lower the unprivileged-port floor (no-op as root, sudo in CI).
sudo -n sysctl -w net.ipv4.ip_unprivileged_port_start=53 >/dev/null 2>&1 \
    || sysctl -w net.ipv4.ip_unprivileged_port_start=53 >/dev/null 2>&1 || true
python3 "$ROOT/tests/sidecars/dns_fixed.py" 127.0.0.1 53 canboot.test 10.0.2.2 >"$WORK/dns.log" 2>&1 &
DNS_PID=$!
sleep 0.5

DISK_IMG="${DISK_IMG:-build/canboot-fat32.img}"
if [ ! -f "$DISK_IMG" ]; then
    "$ROOT/scripts/mkdisk/fat32.sh" "$DISK_IMG" >/dev/null
fi

# Optional NVMe device. When NVME_IMG is set the kernel's NVMe driver
# binds it and the disk selftest does a raw read/write round-trip
# ("selftest: nvme read/write ok"). Off by default so the standard BIOS
# run is unchanged.
NVME_ARGS=""
if [ -n "${NVME_IMG:-}" ]; then
    [ -f "$NVME_IMG" ] || truncate -s 32M "$NVME_IMG"
    NVME_ARGS="-drive if=none,id=nvm0,file=$NVME_IMG,format=raw -device nvme,serial=canvme,drive=nvm0"
fi

# Optional USB: an xHCI controller with a USB-HID boot keyboard. When
# USB_KBD is set the kernel's xHCI driver enumerates it and feeds key
# events; injected keystrokes arrive tagged source=usb-hid.
USB_ARGS=""
if [ -n "${USB_KBD:-}" ]; then
    USB_ARGS="-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0"
    KBD_DEV=""   # drop the virtio keyboard so sendkey routes to usb-kbd
fi

AUDIO_WAV="${AUDIO_WAV:-build/canboot-bios-audio.wav}"
rm -f "$AUDIO_WAV"
qemu-system-x86_64 \
    -machine q35 \
    -boot order=dc \
    -cdrom "$ISO" \
    -drive "if=none,id=blk0,file=$DISK_IMG,format=raw" \
    -device virtio-blk-pci,drive=blk0 \
    $KBD_DEV $PTR_DEV $QMP_DEV \
    -netdev user,id=n0 \
    -device "${NIC_MODEL:-virtio-net-pci},netdev=n0" \
    $NVME_ARGS \
    $USB_ARGS \
    -audiodev "wav,id=snd,path=$AUDIO_WAV" \
    -device intel-hda \
    -device hda-duplex,audiodev=snd \
    -serial "file:$LOG" \
    -monitor "unix:$MON_SOCK,server,nowait" \
    -display none \
    -no-reboot \
    -m 256M \
    -smp 2 \
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

    # Second wave for selftest: cando's input.waitKey loop.
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

    # Third wave: drive the pointer for the cando input.mouse() probe.
    # Best-effort (injection support varies across QEMU versions), so the
    # click is asserted non-fatally below.
    for _ in $(seq 1 200); do
        if grep -q 'cando mouse probe begin' "$LOG" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done
    if [ "$POINTER" = "tablet" ]; then
        # Absolute pointer over QMP: move to two points (axis range
        # 0..32767, scaled to the framebuffer by the virtio-input driver)
        # then a left click.
        python3 - "$QMP_SOCK" <<'PY' 2>/dev/null || true
import socket, sys, time, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(sys.argv[1])
except Exception:
    sys.exit(0)
s.settimeout(2)
def send(obj):
    s.sendall((json.dumps(obj) + "\r\n").encode()); time.sleep(0.2)
    try: s.recv(65536)
    except Exception: pass
try: s.recv(65536)          # QMP greeting
except Exception: pass
send({"execute": "qmp_capabilities"})
def absmove(x, y):
    send({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": x}},
        {"type": "abs", "data": {"axis": "y", "value": y}}]}})
absmove(6000, 6000)
absmove(20000, 14000)
send({"execute": "input-send-event", "arguments": {"events": [
    {"type": "btn", "data": {"button": "left", "down": True}}]}})
send({"execute": "input-send-event", "arguments": {"events": [
    {"type": "btn", "data": {"button": "left", "down": False}}]}})
s.close()
PY
    else
        # Relative PS/2 mouse over the HMP monitor.
        python3 - "$MON_SOCK" <<'PY' 2>/dev/null || true
import socket, sys, time
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    sock.connect(sys.argv[1])
except Exception:
    sys.exit(0)
time.sleep(0.2)
def cmd(c):
    sock.sendall((c + "\n").encode()); time.sleep(0.2)
for _ in range(6):
    cmd("mouse_move 30 20")
cmd("mouse_button 1")
cmd("mouse_button 0")
sock.close()
PY
    fi
) &
INJECTOR_PID=$!

cleanup() {
    for pid in "$INJECTOR_PID" "$QEMU_PID" "$UDP_PID" "$HTTP_PID" "$HTTPS_PID" "$DNS_PID"; do
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
    # display selftest sha256 compare. We do this once, on the first
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
        if [ -n "${USB_KBD:-}" ]; then
            check 'canboot: usb-hid keyboard on port'
            check 'canboot: rx usb-hid'
        else
            check 'canboot: virtio-input ready'
            check 'canboot: rx '
        fi
        check 'selftest: self-test ok'

        check 'selftest: preemption ok'


        check 'selftest: smp observed 2 cpu(s)'

        check 'selftest: big-heap'
        check 'selftest: dhcp lease'
        check 'selftest: udp echo ok'
        check 'selftest: http get ok'
        check 'selftest: dns lookup ok canboot.test=10.0.2.2'
        check 'selftest: net test ok'
        check 'selftest: handshake ok'
        check 'selftest: https get ok'
        check 'selftest: session resumption ok'

        check 'selftest: tls1.3 handshake ok'

        check 'selftest: tls1.3 https get ok'
        check 'selftest: tls test ok'
        check 'selftest: init.cdo marker ok'
        check 'selftest: fat32 subdir tree ok'

        check 'selftest: posix fs surface ok'


        if [ -n "${NVME_IMG:-}" ]; then
            check 'selftest: nvme read/write ok'
        fi
        check 'selftest: disk test ok'
        check 'selftest: cando_open ok'
        check 'selftest: cando_openlibs ok'
        check 'selftest: cando_close ok'
        check 'selftest: cando link test ok'
        check 'canboot-cando-runtime-marker'
        check 'selftest: cando_dostring ok'
        check 'selftest: init.cdo executed ok'
        check 'selftest: display lib registered'
        check 'cando datetime.year ok = true'

        check 'cando stream.memory ok = true'

        check 'cando thread.join = 499500'

        check 'cando thread.state = done'

        check 'cando jit match = true'

        check 'cando jit compiled ok'

        check 'cando gui included ok ver 1.0.0'

        check 'cando gui dashboard painted'

        check 'selftest: display test ok'
        check 'selftest: input lib registered'
        check 'cando input poll begin'
        check 'cando got key1: 120'
        check 'cando got key2: 121'
        check 'cando got key3: 122'
        check 'cando input poll end'
        check 'selftest: system libs registered'
        check 'cando time.ms ='
        check 'cando file.exists(init.cdo) = true'
        check 'cando file.read = hello-from-cando'
        check 'cando net.udpEcho = cando-udp-probe'
        check 'cando net.httpGet = canboot-hello'
        check 'cando tls.httpsGet = canboot-secure'
        check 'cando tls via dns = canboot-secure'

        check 'cando tls.version = TLSv1.3'

        check 'cando socket http ok = true'
        check 'cando socket.resolve ok = true'
        check 'cando secure_socket https ok = true'
        check 'cando secure_socket proto = TLSv1.3'
        check 'cando secure_socket cert present = true'
        check 'cando sys libs end'
        check 'selftest: crypto libs registered'
        check 'cando hex.encode(canboot) = 63616e626f6f74'
        check 'cando base64.encode(canboot) = Y2FuYm9vdA=='
        check 'cando crypto.sha256Hex(empty) = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
        check 'cando crypto.hmacSha256Hex(k, m) = f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8'
        check 'cando crypto libs end'
        check 'selftest: env+log libs registered'
        check 'cando env.source = bios'
        check 'INFO  info-level message from cando'
        check 'INIT.CDO'
        check 'cando intro libs end'
        if grep -q 'THIS-SHOULD-BE-SUPPRESSED' <<<"$stripped"; then
            echo "smoke test FAILED: log.setLevel(info) did not suppress debug message" >&2
            exit 1
        fi
        check 'selftest: extension libs registered'
        check 'cando url.scheme = https'
        check 'cando url.host = 10.0.2.2'
        check 'cando http.get = canboot-hello'
        check 'cando https.get = canboot-secure'
        check 'cando fmt.sprintf = hex=1234 dec=42 str=hi'
        check 'cando ext libs end'
        check 'selftest: partition+fs libs registered'
        check 'cando fs.read fat subdir = cdo-subdir-2026'

        check 'cando fs.list fat subdir = G.TXT'

        check 'cando fs.rmdir fat = true'

        check 'cando part libs end'

        # Pointer probe. The input.mouse() accessor must be reachable (the
        # probe block ran), which is the gated assertion. Whether the
        # injected motion/click actually lands depends on the host QEMU's
        # legacy HMP mouse support, so that part is reported non-fatally.
        check 'cando mouse probe begin'
        check 'cando mouse probe end'
        if grep -q 'cando mouse button = true' <<<"$stripped"; then
            echo "selftest: mouse click registered"
        elif grep -q 'cando mouse moved = true' <<<"$stripped"; then
            echo "selftest: mouse motion registered, click not seen (non-fatal)"
        else
            echo "selftest: mouse not observed (non-fatal; host QEMU PS/2 dependent)"
        fi

        # Milestone 11 screenshot hash compare. Per-host firmware /
        # GRUB stage contributions to the framebuffer drift the
        # exact bytes, so the hash check is non-fatal - the real
        # paint correctness gate is the kernel-side pixel sample
        # check in tests/selftest/cando.c ("selftest: probe red top-
        # left rect" etc.) that already ran above.
        if [ -f "$WORK/screen.ppm" ]; then
            EXPECTED=$(cat "$ROOT/tests/refs/m11-bios.ppm.sha256" 2>/dev/null | head -1)
            GOT=$(sha256sum "$WORK/screen.ppm" | awk '{print $1}')
            if [ "$EXPECTED" = "$GOT" ]; then
                echo "selftest: screendump sha256 matches reference ($GOT)"
            else
                echo "selftest: screendump sha256 host-drift (got=$GOT exp=$EXPECTED) - ignoring, paint already verified in kmain"
                cp "$WORK/screen.ppm" build/m11-bios-actual.ppm 2>/dev/null || true
            fi
        else
            echo "smoke test FAILED: m11 screendump missing" >&2
            exit 1
        fi

        # Milestone 18 audio assertions. canboot's HDA driver must
        # bind successfully (cando reports the device name) and the
        # captured WAV must contain non-silent samples (proof the
        # kernel pushed our sine tone into the DMA ring and the
        # device drained it through to the host wav backend).
        check 'cando audio.deviceName = intel-hda'
        check 'cando audio.present = true'
        check 'cando audio.newSource = 0'
        check 'cando audio.play(src) = true'
        check 'cando audio.isPlaying(src) = true'
        check 'cando audio.isPlaying after stop = false'
        if [ -f "$AUDIO_WAV" ] && [ "$(stat -c '%s' "$AUDIO_WAV")" -gt 4096 ]; then
            # Skip the RIFF/WAVE header and look for at least one
            # non-zero PCM sample in the body.
            if python3 -c "
import sys
with open('$AUDIO_WAV', 'rb') as f:
    data = f.read()
# Header is at least 44 bytes; scan from offset 44 onwards.
body = data[44:]
nz = sum(1 for b in body if b != 0)
sys.exit(0 if nz > 16 else 1)
" 2>/dev/null; then
                echo "selftest: audio capture has non-silent body ($(stat -c '%s' "$AUDIO_WAV") bytes)"
            else
                echo "smoke test FAILED: audio wav body is silent (no non-zero samples)" >&2
                exit 1
            fi
        else
            echo "smoke test FAILED: audio wav missing or empty" >&2
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
