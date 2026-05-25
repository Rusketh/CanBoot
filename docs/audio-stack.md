# Audio stack

End-to-end flow from a script's `audio.play(src)` to the host's
speakers:

```
   cando script
        │
        ▼
   audio.* bindings              (cando_port/lib/audio.c)
        │
        ▼
   Mixer (additive, Q15 fixed-point, saturating clamp)
        │
        ▼
   hal_audio_write(int16_t *, n_frames)
        │
        ▼
   ┌─── x86_64 ───┐         ┌── aarch64 ──┐
   │ Intel HDA    │         │ virtio-snd  │
   │ (intel_hda.c)│         │(virtio_snd.c)│
   └──────────────┘         └─────────────┘
        │                          │
        ▼                          ▼
    QEMU ich9-intel-hda /     QEMU virtio-sound-pci
    real motherboard HDA       (or AAVMF passthrough)
        │                          │
        ▼                          ▼
    Host audio backend         Host audio backend
    (`-audiodev wav,...` etc.)
```

## Source lifecycle

1. **`audio.newSource(bytes)`** triggers `decode_to_hal()` in
   `cando_port/lib/audio.c`:

   - Sniff format: `RIFF...WAVE` -> WAV decoder (inline);
     `ID3` / MPEG frame sync -> `canboot_mp3_decode` from
     `cando_port/vendor_glue/minimp3/decoder.c`.
   - Decode to s16 interleaved PCM at whatever rate/channels the
     source happened to use.
   - Nearest-neighbour resample to the HAL's fixed format
     (44.1 kHz stereo s16) if the source rate or channel count
     differ.

   The result is `malloc`'d per-source PCM that lives until
   `audio.release(src)`. Each second of audio costs ~176 KiB.

2. **`audio.play(src)`** sets the slot's `playing` flag and kicks
   the mixer.

3. **Mixer pump** (`canboot_audio_pump`):

   ```
   For each chunk (default 2048 frames ≈ 46 ms):
     - Zero a wide int32 mix buffer.
     - For each active source:
         compute effective gain = source.volume * master.volume (Q15)
         For each frame:
             advance source.pos (wrap on loop, stop at end)
             mix += sample * gain >> 15
     - Saturate mix into int16
     - hal_audio_write(out, chunk)
   ```

   The pump fires from `audio.play`, `audio.resume`, `audio.update`,
   and — for "fire and forget" scripts — from inside `time.ms`,
   `time.sleep`, `input.waitKey`.

4. **HAL backend** writes into a ring buffer that the device drains
   via DMA.

## HAL backend details

### Intel HDA (`hal/audio/intel_hda.c`)

- **Codec verb exchange** via the legacy Immediate Command path
  (ICOI/ICII/ICIS at BAR0+0x60). The CORB/RIRB DMA ring is set up
  by the spec but not used by canboot — one-shot probe + a small
  number of widget verbs make the polled immediate path much
  cheaper to implement.

- **Codec topology walk**: `STATESTS` -> first responding codec
  -> root node walk for audio function group (AFG)
  -> AFG sub-node walk for first audio output converter +
  first output-capable pin
  -> set converter stream/channel/format, enable EAPD on the pin,
  unmute output amps at gain 0x50.

- **Single DMA stream**: 32 KiB ring split into two 16 KiB BDL
  entries. Stream tag 1, format `SD_FMT = 0x4011` (44.1 kHz, 16-bit,
  2 channels). `SD_CTL` `RUN` bit set; polled `SDx_LPIB` for write
  pacing.

### virtio-sound (`hal/audio/virtio_snd.c`)

- **PCI ID** vendor `0x1AF4` modern device `0x1059`.

- **Virtqueues**: controlq (idx 0) + txq (idx 2). eventq + rxq stay
  un-set-up — we don't capture and don't consume async events.

- **Setup**: device config exposes `streams` count.
  `VIRTIO_SND_R_PCM_INFO` queries get the per-stream direction;
  pick the first OUTPUT stream. `SET_PARAMS` -> `PREPARE` -> `START`.

- **TX**: 3-descriptor chain per slot — `virtio_snd_pcm_xfer`
  (driver→device readable) + PCM data (driver→device readable) +
  `virtio_snd_pcm_status` (device→driver writable). 8 slots × 16 KiB
  each pre-allocated, so the writer can stay batched + the device
  fed during heavy bursts.

### Stub backend (`hal/audio/audio_stub.c`)

Weak-symbol fallbacks. Every HAL audio entry point exists; the stub
versions accept inputs and return success without doing any I/O. The
linker picks the strong backend (`intel_hda.c` or `virtio_snd.c`)
when it builds for that target.

## What you hear vs. what canboot sends

QEMU's `-audiodev wav,path=cap.wav` records exactly what the audio
device drained. Sample rate / channel count / bit depth in the wav
file match what canboot wrote: 44.1 kHz stereo s16. The host audio
backend's own resampling / mixing doesn't apply to `wav` capture.

That's why the smoke test can byte-compare the captured wav against
expected content — there's no host-side processing in between.

## Adding a backend

`hal/audio/audio_stub.c` shows the surface. Any backend that
provides strong `hal_audio_*` symbols + correctly drives a sound
device given 44.1 kHz stereo s16 frames in interleaved L,R,L,R,...
order will be linked over the stub. Add the source file to
`CMakeLists.txt` per-target, no changes anywhere else.

## Source files

| Path | What |
|------|------|
| `cando_port/lib/audio.c` | cando bindings + mixer |
| `cando_port/vendor_glue/stb/image.c` | unrelated; left for reference (image) |
| `cando_port/vendor_glue/minimp3/decoder.c` | MP3 decoder wrapper |
| `hal/include/hal/audio.h`     | HAL surface |
| `hal/audio/audio_stub.c`      | weak fallback backend |
| `hal/audio/intel_hda.c`       | x86_64 backend |
| `hal/audio/virtio_snd.c`      | aarch64 backend |
