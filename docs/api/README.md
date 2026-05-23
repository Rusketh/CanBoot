# CanBoot CanDo binding reference

Per-namespace reference for the CanDo libraries CanBoot exposes on
bare metal. Grouped by subsystem; each page contains every namespace
in that group with its full API.

| Page | Namespaces |
|------|-----------|
| **[io.md](io.md)** — interactive surfaces | `input`, `display`, `fb` |
| **[storage.md](storage.md)** — disks + filesystems | `disk`, `partition`, `fs`, `file`, `pci` |
| **[net.md](net.md)** — networking | `net`, `http`, `https`, `tls`, `url` |
| **[crypto.md](crypto.md)** — hashing + entropy + encoding | `crypto`, `random`, `hex`, `base64` |
| **[media.md](media.md)** — image + audio | `image`, `audio` |
| **[system.md](system.md)** — boot environment + clocks + logging | `env`, `time`, `log`, `fmt` |

The bindings live under `cando_port/lib/` in the source tree; the
underlying HAL / runtime they sit on top of is documented in
[`../hal.md`](../hal.md), [`../networking.md`](../networking.md),
[`../filesystems.md`](../filesystems.md),
[`../audio-stack.md`](../audio-stack.md).
