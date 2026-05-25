# fmt — printf + binary packers

String formatting plus little-endian binary packers and a sine-wave
generator for composing binary payloads (WAV headers, test tones)
without a separate hex round-trip.

## `fmt.sprintf(format, ...args) -> string`

Like C's printf. Supported conversions: `%s`, `%d`/`%i`, `%u`, `%x`,
`%X`, `%f`, `%%`. Width / precision specifiers are passed through to
the underlying picolibc `snprintf`.

```cdo
fmt.sprintf("addr=%s port=%d", host, port)
// -> "addr=10.0.2.2 port=8443"
```

`%f` and `%g` of a floating-point value render normally — picolibc
is built with `format-default=double`. Scripts can pass
float-returning calls like `audio.getDuration(s)`,
`audio.getVolume(s)`, or `random.float()` straight into
`fmt.sprintf("%.2f", ...)`.

## `fmt.u16le(n) -> string`

Pack `n` as 2 raw little-endian bytes. Use to compose binary file
headers (WAV, BMP, etc.) without a separate base-16 round-trip.

## `fmt.u32le(n) -> string`

Pack `n` as 4 raw little-endian bytes.

## `fmt.sineWave16(freq, rate, n_samples) -> string`

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

## Behaviour

- `u16le` / `u32le` clamp to the low 16/32 bits — overflow silently
  wraps.
- `sineWave16` amplitude is fixed at ~91% of full-scale (`+30000`
  peak) to leave a little headroom for the audio mixer's master
  volume + cumulative source mixing.

## See also

- [`audio`](audio.md) — consume the PCM `sineWave16` produces
- [`log`](log.md) — `fmt.sprintf` to build log messages
