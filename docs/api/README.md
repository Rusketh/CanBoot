# CanBoot CanDo binding reference

Per-library reference for the CanDo libraries CanBoot exposes on bare
metal. Each library is a global object with method-style calls,
available to `/init.cdo` once the kernel has brought up the HAL and
opened the VM.

The bindings live under `cando_port/lib/` in the source tree; the
underlying HAL / runtime they sit on top of is documented in
[`../hal.md`](../hal.md), [`../networking.md`](../networking.md),
[`../filesystems.md`](../filesystems.md), and
[`../audio-stack.md`](../audio-stack.md).

## Interactive surfaces

| Library | What it does |
|---------|--------------|
| [input](input.md)     | Polled keyboard input (poll / waitKey / flush) |
| [display](display.md) | Framebuffer painter: clear, fillRect, line, text, image, pixel |
| [fb](fb.md)           | Scanout flush / present for explicit-present devices |

## GUI toolkit

| Library | What it does |
|---------|--------------|
| [gui](gui.md) | Retained-mode widget toolkit (`include("/gui.cdo")`) over `display` + `input` |

## Storage

| Library | What it does |
|---------|--------------|
| [disk](disk.md)           | Raw block device enumeration + LBA read/write |
| [partition](partition.md) | GPT + MBR partition table read **and write** |
| [fs](fs.md)               | Filesystem-aware read/write/delete/mkfs (FAT32, NTFS, ext2/3/4, ISO9660) |
| [file](file.md)           | Single-disk root-dir convenience (read/write/list/exists/size) |
| [pci](pci.md)             | PCI(e) bus walk: count, list with vendor/device/class |

## Networking

| Library | What it does |
|---------|--------------|
| [net](net.md)     | UDP echo + HTTP GET over lwIP, DNS lookup |
| [http](http.md)   | URL-aware cleartext HTTP GET + status |
| [https](https.md) | HTTPS GET via Mbed TLS (TLS 1.2) |
| [tls](tls.md)     | TLS-protected fetch (symmetric naming with `net.*`) |
| [url](url.md)     | URL parse: scheme / host / port / path |

## Crypto + encoding

| Library | What it does |
|---------|--------------|
| [crypto](crypto.md) | Hashes, HMAC, PBKDF2/HKDF, AES-CBC/CTR, timing-safe compare |
| [random](random.md) | Hardware RNG + jitter fallback: bytes / int / float / hex / uuid |
| [hex](hex.md)       | Hex encode / decode |
| [base64](base64.md) | RFC 4648 base64 encode / decode |

## Media

| Library | What it does |
|---------|--------------|
| [image](image.md) | PNG / JPG / BMP decode + scaled blit (stb_image) |
| [audio](audio.md) | LOVE 2D-shaped Source / mixer / volume / loop (WAV + MP3) |

## System

| Library | What it does |
|---------|--------------|
| [env](env.md)     | Boot source + framebuffer + memory-map introspection |
| [os](os.md)       | CanDo `os.*` drop-in (bare-metal semantics) |
| [time](time.md)   | Monotonic clock: ms / us / ticks / sleep |
| [log](log.md)     | Levelled logging with timestamps |
| [fmt](fmt.md)     | sprintf + binary little-endian packers + sine-wave generator |
| [Error](error.md) | Structured `(value, err)` error class (code / message / cause) |
