# Release artifacts

CanBoot publishes prebuilt boot images on every push to `main` (rolling
nightly) and every `v*` tag (stable release). All artifacts are produced
by the same CI workflow that runs the smoke tests, so anything you
download has booted at least once under QEMU before being uploaded.

## Where

- **Stable** — <https://github.com/Rusketh/CanBoot/releases/latest>
- **Nightly** — <https://github.com/Rusketh/CanBoot/releases/tag/nightly>
  (overwritten on every push to `main`)

## What's in a release

Each release contains the following nine files. Pick the one that
matches your target.

| File | Architecture | Firmware | Form |
|------|--------------|----------|------|
| `canboot-x86_64.elf`            | x86_64  | BIOS  | Multiboot2 ELF (use with GRUB chainload) |
| `canboot-x86_64-bios.iso`       | x86_64  | BIOS  | Bootable hybrid ISO (USB + CD) |
| `canboot-x86_64-uefi.efi`       | x86_64  | UEFI  | PE/COFF UEFI application |
| `canboot-x86_64-uefi.iso`       | x86_64  | UEFI  | Bootable UEFI ISO (ESP + BOOTX64.EFI) |
| `canboot-aarch64.elf`           | aarch64 | none  | Kernel ELF (`-kernel` style) |
| `canboot-aarch64.bin`           | aarch64 | none  | Flat binary (no ELF wrapper) |
| `canboot-aarch64-uefi.efi`      | aarch64 | UEFI  | PE/COFF for AAVMF / EDK2 |
| `canboot-aarch64-uefi.img`      | aarch64 | UEFI  | Raw FAT32 ESP image with BOOTAA64.EFI |
| `SHA256SUMS.txt`                | -       | -     | SHA-256 of every file above |

## Verifying

The release workflow generates a `SHA256SUMS.txt` from every staged
artifact before publishing. Drop it next to the downloaded files and:

```sh
sha256sum -c SHA256SUMS.txt
```

Expected output is one `OK` line per file present.

## Picking the right artifact

```
            +-- target firmware ------+
            |                         |
        BIOS only?               UEFI capable?
            |                         |
            v                         v
   canboot-x86_64-bios.iso    +- which arch? -+
                              |               |
                         x86_64             aarch64
                              |               |
                              v               v
                  canboot-x86_64-uefi.iso  canboot-aarch64-uefi.img
                  (or .efi to drop on      (or .efi to drop on existing
                   an existing ESP)         aarch64 ESP)
```

When in doubt:

- **You're on a PC made after ~2012** → x86_64 UEFI.
- **Old PC / virtual machine without OVMF** → x86_64 BIOS.
- **Raspberry Pi 4 / Apple silicon under Asahi / aarch64 board with UEFI** → aarch64 UEFI.
- **aarch64 dev board you boot via `-kernel` or a board-vendor wrapper** → aarch64 direct (`.bin` or `.elf`).

## Tagging a release

Releases are driven entirely by git tags. Maintainers cut a stable
release by pushing a `v*` tag:

```sh
git tag -a v0.1.0 -m "first public preview"
git push origin v0.1.0
```

The CI release workflow picks up the tag, runs the full matrix
(x86_64 BIOS, x86_64 UEFI, aarch64 direct, aarch64 UEFI), and uploads
the nine artifacts plus `SHA256SUMS.txt` to a GitHub release named
after the tag.

For testing the release pipeline without cutting a real version, use
the workflow's `workflow_dispatch` trigger with an explicit `tag`
input — that pins the publish step to whatever label you pass.

## Nightly

Every push to `main` re-runs the release workflow and overwrites the
`nightly` tag in place. Old nightlies are not retained; the URL stays
stable so links don't rot, but the bytes change. Treat anything
downloaded from `nightly` as a moving target.

## CI matrix

Each release artifact is produced by an independent matrix job. The
table below maps each job to what it builds and tests.

| Job              | Builds                                | Smoke test runner |
|------------------|---------------------------------------|--------------------|
| `x86_64 bios`    | `canboot-x86_64.elf`, BIOS ISO        | `tests/run-qemu-bios.sh` |
| `x86_64 uefi`    | UEFI `.efi`, UEFI ISO                 | `tests/run-qemu-uefi.sh` |
| `aarch64 direct` | aarch64 `.elf` + flat `.bin`          | `tests/run-qemu-aarch64.sh` |
| `aarch64 uefi`   | aarch64 UEFI `.efi` + ESP `.img`      | `tests/run-qemu-aarch64-uefi.sh` |

A release publishes only when all four matrix jobs report green. If any
target fails to boot or fails an assertion, the workflow's `publish`
job never runs and the previous release stays in place.
