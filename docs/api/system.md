# system — boot environment, clocks, logging, formatting

The small set of namespaces every script needs: what the loader handed
us, what time it is, where to write log lines, and how to format
binary payloads.

- [`env`](#env) — boot environment introspection
- [`time`](#time) — monotonic clock + sleep
- [`log`](#log) — levelled logging
- [`fmt`](#fmt) — printf, binary packers, sine-wave generator

---

## env

What the loader handed us: framebuffer format, memory map summary,
boot source.

### `env.source() -> string`

How the kernel was booted. `"bios"`, `"uefi"`, or `"direct"` (aarch64
`-kernel` path).

### `env.fbFormat() -> string`

Framebuffer encoding. `"rgb"` for standard packed-pixel RGBA-like
formats, `"none"` when no framebuffer is available.

### `env.fbWidth() -> number`

Framebuffer width in pixels. `0` when there's no framebuffer.

### `env.fbHeight() -> number`

Framebuffer height in pixels.

### `env.mmapCount() -> number`

Number of entries in the boot memory map.

### `env.usableBytes() -> number`

Sum of `usable` regions in the memory map. Useful for reporting
"we have N MB of RAM" without parsing the full map.

```cdo
print("env.source =", env.source());
print("env.fbFormat =", env.fbFormat());
print("env.fbWidth =", env.fbWidth());
print("env.fbHeight =", env.fbHeight());
print("env.usableBytes =", env.usableBytes());
```

### Behaviour

- Values are read from `boot_info` once at kmain time. Subsequent
  calls return the same numbers.

---

## time

Backed by the TSC-calibrated clock that lwIP's `sys_arch.c` uses.
Calibration runs at boot against the i8254 PIT (x86_64) or the
architected `cntfrq_el0` register (aarch64).

### `time.ms() -> number`

Milliseconds since boot. Wraps after ~49 days. Same value as
lwIP's `sys_now()`.

### `time.us() -> number`

Microseconds since boot. 64-bit value held in a JS double, so the
practical precision starts to drop after ~285 years. You're fine.

### `time.ticks() -> number`

Raw counter ticks since boot. Architecture-specific units — divide
by `time.ticksHz()` to get seconds.

### `time.ticksHz() -> number`

Frequency of the tick counter in Hz. On QEMU x86_64 typically
~2.5 GHz; on QEMU aarch64 virt typically `62500000`.

### `time.sleep(ms)`

Busy-wait for `ms` milliseconds. Cooperatively pumps lwIP's network
driver and the audio mixer between iterations, so neither stalls
while a script sleeps.

```cdo
time.sleep(1000);   // 1-second pause
```

### Behaviour

- The clock is **monotonic** — it never goes backwards. There's no
  wall-clock time in CanBoot today (no RTC bring-up); use it for
  measuring intervals, not for displaying "current time".
- `time.sleep(0)` returns immediately but still does one pump cycle,
  which makes it useful for "let the audio + network catch up" in a
  tight loop.
- Audio + network pump from `time.ms` and `time.sleep` automatically.
  Scripts that loop calling `time.ms` get continuous audio playback
  even with no explicit `audio.update()` call.

---

## log

Timestamped log lines with a per-process level filter. Output goes
to the same serial console as `print()`.

### `log.setLevel(name) -> bool`

Set the minimum level. One of `"debug"`, `"info"`, `"warn"`,
`"error"`. Messages below the threshold are dropped.

```cdo
log.setLevel("info");   // discard debug, keep info+
```

### `log.debug(msg)`, `log.info(msg)`, `log.warn(msg)`, `log.error(msg)`

Log at the given level. Output format:

```
[     12345] INFO  msg goes here
```

The `[NNNNN]` is `time.ms()` at log time, padded to 10 columns.

### `log.getLevel() -> string`

Currently active level name.

### Behaviour

- Default level is `"debug"` — everything is logged.
- Levels are case-insensitive on `setLevel`.
- The full `time.ms()` value is stamped, so logs can be cross-referenced
  against other prints / kernel output without separate sequencing.

---

## fmt

### `fmt.sprintf(format, ...args) -> string`

Like C's printf. Supported conversions: `%s`, `%d`/`%i`, `%u`, `%x`,
`%X`, `%f`, `%%`. Width / precision specifiers are passed through to
the underlying picolibc `snprintf`.

```cdo
fmt.sprintf("addr=%s port=%d", host, port)
// -> "addr=10.0.2.2 port=8443"
```

⚠ Picolibc is built with `format-default=integer`, so `%f` of a
floating-point value renders as the literal `*float*` placeholder.
For now, convert to integer milliseconds (or some other integer
unit) before printing.

### `fmt.u16le(n) -> string`

Pack `n` as 2 raw little-endian bytes. Use to compose binary file
headers (WAV, BMP, etc.) without a separate base-16 round-trip.

### `fmt.u32le(n) -> string`

Pack `n` as 4 raw little-endian bytes.

### `fmt.sineWave16(freq, rate, n_samples) -> string`

Generate `n_samples` of 16-bit signed PCM sine wave at `freq` Hz
against sample rate `rate`. Returned as raw little-endian bytes
suitable for splicing directly into a RIFF/WAVE `data` chunk.

```cdo
// Build a 0.25 s 440 Hz mono sine in RIFF form, end-to-end:
VAR sr   = 44100;
VAR ns   = 11025;            // 0.25 s
VAR data_sz = ns * 2;        // 16-bit samples
VAR hdr  = "RIFF" + fmt.u32le(36 + data_sz)
         + "WAVEfmt "
         + fmt.u32le(16) + fmt.u16le(1) + fmt.u16le(1)
         + fmt.u32le(sr) + fmt.u32le(sr * 2)
         + fmt.u16le(2) + fmt.u16le(16) + "data" + fmt.u32le(data_sz);
VAR tone = fmt.sineWave16(440, sr, ns);
VAR src  = audio.newSource(hdr + tone);
audio.play(src);
```

### Behaviour

- `u16le` / `u32le` clamp to the low 16/32 bits — overflow silently
  wraps.
- `sineWave16` amplitude is fixed at ~91% of full-scale (`+30000`
  peak) to leave a little headroom for the audio mixer's master
  volume + cumulative source mixing.
