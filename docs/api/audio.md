# `audio` — Source / mixer / playback

LOVE 2D-shaped audio API. Decode a WAV or MP3 into an opaque `Source`
handle, then play, pause, seek, set volume, loop. Multiple Sources mix
together additively into a single output stream that drains through the
HAL audio backend (Intel HDA on x86_64, virtio-sound on aarch64).

## Quick recap

```cdo
VAR sfx = audio.newSource(fs.read(0, 0, "/click.wav"));
VAR bgm = audio.newSource(fs.read(0, 0, "/music.mp3"));

audio.setVolume(bgm, 0.4);   // 40% per-source gain
audio.setLooping(bgm, true);
audio.play(bgm);             // returns immediately
audio.play(sfx);             // layered on top
audio.activeCount();         // -> 2

audio.setVolume(0.8);        // master gain (one arg = master)
audio.pause();               // no arg = pause everything
audio.resume();
audio.stop();
```

## Creating a Source

### `audio.newSource(bytes) -> handle`

Decode the bytes into a Source. Auto-detects WAV (`RIFF`...`WAVE` header)
or MP3 (`ID3` tag or MPEG frame sync). Resamples to the HAL's fixed
44.1 kHz stereo s16 format up front so playback is just a flag flip.

Returns an integer handle ≥ 0 on success, `-1` on decode failure or if
all 8 source slots are in use.

Memory cost: roughly **176 KiB per second of audio**. A 10-second clip
holds ~1.7 MiB of decoded PCM resident.

```cdo
VAR src = audio.newSource(wav_bytes_or_mp3_bytes);
IF (src == -1) {
    print("decode failed or pool exhausted");
}
```

## Per-source playback

### `audio.play(src) -> bool`

Start playback from the current position. If the source was paused,
resumes. If it was stopped, plays from the beginning. Returns `true`
on success, `false` if `src` isn't a live handle.

Returns immediately — the mixer + HAL drain the samples asynchronously.

### `audio.stop(src) -> bool`

Stop playback and rewind to frame 0. Subsequent `play()` starts from
the beginning.

### `audio.pause(src) -> bool`

Stop playback but keep the current position. `resume()` continues
from where pause was called.

### `audio.resume(src) -> bool`

Resume from a paused state. Equivalent to `play()` when the source was
not paused.

## Per-source state

### `audio.isPlaying(src) -> bool`

`true` iff the source is actively producing samples. Returns `false`
when paused or stopped.

### `audio.isPaused(src) -> bool`

`true` iff `pause()` was called and `resume()` / `stop()` hasn't been.

### `audio.setVolume(src, v) -> bool`

Per-source gain, `0.0` (silent) to `1.0` (full). Multiplied with the
master volume at mix time in Q15 fixed-point.

```cdo
audio.setVolume(bgm, 0.4);
```

### `audio.getVolume(src) -> number`

Current per-source gain.

### `audio.setLooping(src, b) -> bool`

When `true`, the source wraps back to frame 0 when it hits the end.
When `false`, the source transitions to "stopped" at the end and
`isPlaying()` returns `false`.

### `audio.isLooping(src) -> bool`

Current looping flag.

### `audio.getDuration(src) -> number`

Total length of the source in **seconds** (floating-point).

### `audio.tell(src) -> number`

Current playback position in **seconds**. `0.0` if the source has
never played or was just stopped.

### `audio.seek(src, t) -> bool`

Jump to time `t` seconds into the source. Clamped to
`[0, getDuration]`. Works on playing, paused, or stopped sources.

## Cleanup

### `audio.release(src) -> bool`

Free the source's decoded PCM buffer and recycle its slot. The handle
becomes invalid; subsequent calls on it return `false`.

Always release sources you won't play again — they otherwise occupy
both a slot (max 8) and a chunk of heap proportional to their
duration.

## Master controls

### `audio.setVolume(v) -> bool`

**Overload by argc**. Called with a single number, sets the master
volume `[0..1]`. The mixer multiplies every source by this before
clipping.

```cdo
audio.setVolume(0.8);        // master gain
audio.setVolume(bgm, 0.4);   // per-source gain
```

### `audio.getVolume() -> number`

Master volume. Without an argument.

### `audio.stop()`, `audio.pause()`, `audio.resume()`

Called with no args, operate on **every** live source.

```cdo
audio.stop();    // stop and rewind everything
audio.pause();   // pause everything that's playing
audio.resume();  // resume everything paused
```

## Mixer pump

### `audio.update([nframes]) -> bool`

Explicitly run the mixer for `nframes` frames (default 2048 ≈ 46 ms).
Pushes the mixed output into the HAL ring.

You normally don't need this. The mixer auto-pumps from:

- `audio.play` / `audio.resume` (initial kick)
- `time.ms` / `time.sleep` (every call)
- `input.waitKey` (every iteration of its poll loop)

So scripts that "play and move on into the input loop" get continuous
playback for free. Use `audio.update` explicitly only in tight loops
that do none of those things.

## Device introspection

### `audio.present() -> bool`

`true` iff the HAL audio backend successfully bound a sound device at
boot. `false` on platforms without HDA / virtio-sound, or when the
backend's init failed (no codec response, virtqueue setup error,
etc.). The audio API still accepts calls in this state — `newSource`
+ `play` succeed, position advances normally — the samples just drop
on the floor.

### `audio.deviceName() -> string`

One of:

- `"intel-hda"` on x86_64 BIOS / UEFI when an HDA controller is on PCI
- `"virtio-snd"` on aarch64 UEFI when `virtio-sound-pci` is attached
- `"none"` when the stub backend is active

### `audio.activeCount() -> number`

Number of sources currently playing (not paused, not stopped). `0..8`.

## Behaviour

- **Multiple concurrent sources** mix additively with `[-32768, 32767]`
  saturation clamp. Loud sources can clip; pull master volume down to
  fix it.
- **Sample rate / channel coercion** happens once at `newSource()`
  time. Mono input is duplicated to stereo; non-44.1 kHz input is
  nearest-neighbour resampled. Quality is "good enough for boot
  chimes", not "good enough for music production".
- **Up to 8 concurrent sources.** A 9th `newSource()` returns `-1`
  until you `release()` one.
- **Up to ~1.5 sec lookahead.** Samples queued into the HAL ring stay
  audible for a fraction of a second even after `audio.stop()`.
  Call `hal_audio_stop()`-equivalent via `audio.stop()` then wait one
  pump cycle if you need a hard cutoff.
- **No pitch / spatial / streaming** — see "Differences from real
  LOVE" below.

## Differences from real LOVE 2D

| LOVE | CanBoot |
|------|---------|
| `Source:method()` calls | Flat function-call style with handle as first arg |
| `love.audio.newSource(path, mode)` | `audio.newSource(bytes)` — read the file yourself with `fs.read` |
| Static + stream modes | Always static — full decode at `newSource` time |
| `source:setPitch(p)` | Not implemented (would need per-source resampling each pump) |
| `source:setPosition(x,y,z)` | Not implemented (no spatial audio) |
| `love.audio.getActiveSourceCount()` | `audio.activeCount()` |

## See also

- [audio-stack.md](../audio-stack.md) — decoder → mixer → HAL → driver pipeline
- [hal.md](../hal.md) — the `hal_audio_*` C surface underneath
- [fmt.md](fmt.md) — `fmt.sineWave16` for synthesising test tones inline
