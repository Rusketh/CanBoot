# Testing

CanBoot's test suite is per-arch QEMU runners that boot the kernel +
init.cdo, drive interactions via the HMP monitor (key injection, screen
capture, audio capture), poll the serial log for milestone markers, and
fail if any assertion misses.

## Runners

| Script | Target |
|--------|--------|
| `tests/run-qemu-bios.sh`         | x86_64 BIOS via GRUB / Multiboot2 |
| `tests/run-qemu-uefi.sh`         | x86_64 UEFI via OVMF |
| `tests/run-qemu-aarch64.sh`      | aarch64 direct via `-kernel` |
| `tests/run-qemu-aarch64-uefi.sh` | aarch64 UEFI via AAVMF |

Each script:

1. Boots an empty FAT32 disk + the test ISO/IMG.
2. Attaches a virtio-keyboard, a virtio-net (with SLIRP), virtio-blk
   for the FAT32 disk + optional NTFS/ext4 test disks, and an audio
   device (`intel-hda` on x86_64, `virtio-sound-pci` on aarch64).
3. Spawns host-side Python sidecars on `127.0.0.1:7777` (UDP echo),
   `:8080` (HTTP), `:8443` (HTTPS with the test CA).
4. Tails the serial log until either:
   - the boot reaches `ok` at the end of `kmain` → check assertions.
   - the timeout fires (default 240 s x86_64, 480 s aarch64).
5. Asserts every milestone marker is present in the log, asserts
   the screendump PPM SHA256 matches a checked-in reference (BIOS
   path, host-deterministic), and that the captured audio WAV has
   non-silent body.

## Per-milestone assertions

A non-exhaustive sample from `tests/run-qemu-aarch64-uefi.sh`:

```
check 'canboot: uefi entry reached (aarch64)'
check 'canboot: calling ExitBootServices (aarch64)'
check 'canboot: kmain reached (aarch64)'
check 'canboot: handshake confirmed (aarch64 milestone-3)'
check 'canboot: pci devs='
check 'canboot: virtio-input present'
check 'milestone 5: self-test ok'
check 'milestone 6: udp echo ok'
check 'milestone 6: http get ok'
check 'milestone 7: handshake ok'
check 'milestone 7: https get ok'
check 'milestone 7: session resumption ok'
check 'milestone 8: init.cdo marker ok'
check 'milestone 9: cando_open ok'
check 'milestone 10: cando_dostring ok'
check 'milestone 11: display test ok'
check 'cando file.exists(init.cdo) = true'
check 'cando net.udpEcho = cando-udp-probe'
check 'cando crypto.sha256Hex(empty) = e3b0c44298fc...'
check 'cando fs.detect(1,0) = ntfs'
check 'cando fs.mkfs ntfs = true'
check 'cando fs.detect after mkfs = ntfs'
check 'cando fs.mkfs ext4 = true'
check 'cando audio.deviceName = virtio-snd'
check 'cando audio.play(src) = true'
```

The `check` function greps for the literal string in the captured
serial log; missing markers exit non-zero and dump the full log for
debugging.

## Host-side cross-validation

Some selftests also verify themselves using independent host tools:

| Selftest | Host check |
|----------|-----------|
| display | `screendump` PPM SHA256 vs checked-in reference (BIOS only — UEFI / aarch64 GOP byte order varies per firmware) |
| audio   | `-audiodev wav,...` capture has >16 non-zero bytes in the PCM body |
| NTFS write    | `ntfscat -f` reads back the marker file canboot wrote |
| NTFS format   | host `mount.ntfs-3g` on the canboot-formatted image |
| ext4 write    | `debugfs dump` reads back the marker |
| ext4 format   | `e2fsck -fy` returns 0 after one auto-fix pass |

## CI

`.github/workflows/ci.yml` runs the same scripts on `ubuntu-latest`
GitHub Actions runners for every push and pull request. The matrix
is `{x86_64-bios, x86_64-uefi, aarch64-direct, aarch64-uefi}` — four
independent jobs, fail-fast off.

The release workflow (`.github/workflows/release.yml`) reuses the
same flow, then publishes the artifacts when all four are green.

## Running tests locally

Same flow as CI:

```sh
# Build everything first
cmake -B build -G Ninja -DCANBOOT_ARCH=x86_64
cmake --build build --target canboot-x86_64 canboot-uefi
bash scripts/mkiso-bios.sh build/canboot-x86_64.elf       build/canboot-x86_64-bios.iso
bash scripts/mkiso-uefi.sh build/canboot-x86_64-uefi.efi  build/canboot-x86_64-uefi.iso

cmake -B build-aarch64 -G Ninja -DCANBOOT_ARCH=aarch64 \
                       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
cmake --build build-aarch64 --target canboot-aarch64-bin canboot-uefi
bash scripts/mkdisk-aarch64-uefi.sh build-aarch64/canboot-aarch64-uefi.efi \
                                    build-aarch64/canboot-aarch64-uefi.img

# Then run the smoke tests
bash tests/run-qemu-bios.sh         build/canboot-x86_64-bios.iso
bash tests/run-qemu-uefi.sh         build/canboot-x86_64-uefi.iso
bash tests/run-qemu-aarch64.sh      build-aarch64/canboot-aarch64.bin
bash tests/run-qemu-aarch64-uefi.sh build-aarch64/canboot-aarch64-uefi.img
```

Each script returns 0 on success and prints the captured serial log
prefixed with `  |` on failure.

## Adding a test

There's no separate test harness — tests are assertions in the QEMU
runner scripts. To exercise a new code path:

1. Add a milestone-like print in the C code, or a `print()` in
   `initramfs/init.cdo`.
2. Add a `check 'literal text'` line in each relevant runner.
3. Run the runner. If the literal isn't in the log, the test fails
   and dumps the full log to stderr.

For complex assertions (binary comparisons, fuzzy matching), do the
work in a `python3 -c` block inside the runner.

## What to do when a test breaks

1. The runner prints the full serial log on failure. Read it.
2. Find the last marker that DID succeed, then look at what comes
   next in the milestone sequence.
3. The runner also uploads `build*/qemu-*.log` and `*.stderr.log` as
   CI artifacts (visible from the GitHub Actions UI under the failed
   run) — those are the same logs you see locally.
4. For audio / display issues, the captured wav / ppm artifact is
   also uploaded; download and inspect.

CI artifacts for failed runs are retained for 7 days; succeeded
builds retain artifacts for 14 days. Past those windows, re-run
locally.
