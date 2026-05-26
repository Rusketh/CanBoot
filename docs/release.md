# Release artifacts

CanBoot publishes prebuilt boot images on every `v*` tag (stable release).
All artifacts are produced by the same CI workflow that runs the smoke
tests, so anything you download has booted at least once under QEMU before
being uploaded.

## Where

- **Stable** — <https://github.com/Rusketh/CanBoot/releases/latest>

## What's in a release

A release contains one self-contained `.zip` per architecture+firmware
target, plus an aggregated checksum file. Pick the one that matches your
target.

| Package | Architecture | Firmware | Boot media |
|---------|--------------|----------|------------|
| `canboot-x86_64-bios.zip`    | x86_64  | BIOS  | hybrid ISO (USB + CD) |
| `canboot-x86_64-uefi.zip`    | x86_64  | UEFI  | UEFI ISO (ESP + BOOTX64.EFI) |
| `canboot-aarch64-direct.zip` | aarch64 | none  | flat `.bin` / `.elf` (`-kernel` style) |
| `canboot-aarch64-uefi.zip`   | aarch64 | UEFI  | raw FAT32 ESP image (BOOTAA64.EFI) |
| `SHA256SUMS.txt`             | -       | -     | SHA-256 of every package above |

Each `.zip` contains:

- `tftp/` — the files a TFTP server hands a PXE client (kernel/EFI + the
  GRUB netboot tree on BIOS) with `init.cdo` at the root.
- the bootable ISO/image for that target (BIOS/UEFI ISO, or aarch64 UEFI
  `.img`; the aarch64 direct package ships the flat `.bin` instead).
- the loose kernel/EFI files for dropping onto an existing ESP or wrapping
  yourself.
- `dnsmasq.conf` + `PXE-README.txt` — a ready-to-edit DHCP+TFTP server
  template and setup notes.
- `SHA256SUMS` — checksums of everything in the package.

## Verifying

The release workflow generates a top-level `SHA256SUMS.txt` over the
published zips. Drop it next to the downloaded files and:

```sh
sha256sum -c SHA256SUMS.txt
```

Each package also carries its own internal `SHA256SUMS` covering the files
inside it.

## Picking the right artifact

- **You're on a PC made after ~2012** → `canboot-x86_64-uefi.zip`.
- **Old PC / virtual machine without OVMF** → `canboot-x86_64-bios.zip`.
- **Raspberry Pi 4 / Apple silicon under Asahi / aarch64 board with UEFI** →
  `canboot-aarch64-uefi.zip`.
- **aarch64 dev board you boot via `-kernel` or a board-vendor wrapper** →
  `canboot-aarch64-direct.zip`.
- **Netbooting any of the above** → use the `tftp/` tree + `dnsmasq.conf`
  inside the matching package; see [running.md](running.md).

## Tagging a release

Releases are driven entirely by git tags. Maintainers cut a stable
release by pushing a `v*` tag:

```sh
git tag -a v0.1.0 -m "first public preview"
git push origin v0.1.0
```

The CI release workflow picks up the tag, runs the full matrix
(x86_64 BIOS, x86_64 UEFI, aarch64 direct, aarch64 UEFI), and uploads the
four per-target zips plus `SHA256SUMS.txt` to a GitHub release named after
the tag.

For testing the release pipeline without cutting a real version, use the
workflow's `workflow_dispatch` trigger with an explicit `tag` input — that
pins the publish step to whatever label you pass.

## CI matrix

Each release package is produced by an independent matrix job. The table
below maps each job to what it builds and tests.

| Job              | Builds                                | Smoke test runner |
|------------------|---------------------------------------|--------------------|
| `x86_64 bios`    | `canboot-x86_64.elf`, BIOS ISO, PXE tree | `tests/run-qemu-bios.sh` |
| `x86_64 uefi`    | UEFI `.efi`, UEFI ISO, PXE tree       | `tests/run-qemu-uefi.sh` |
| `aarch64 direct` | aarch64 `.elf` + flat `.bin`, PXE tree | `tests/run-qemu-aarch64.sh` |
| `aarch64 uefi`   | aarch64 UEFI `.efi` + ESP `.img`, PXE tree | `tests/run-qemu-aarch64-uefi.sh` |

A release publishes only when all four matrix jobs report green. If any
target fails to boot or fails an assertion, the workflow's `publish` job
never runs and the previous release stays in place.
