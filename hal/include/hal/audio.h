#ifndef CANBOOT_HAL_AUDIO_H
#define CANBOOT_HAL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * HAL audio surface. Exposes a single output stream that scripts can
 * push 16-bit signed-PCM samples at. The backend selects an actual
 * device at boot (Intel HDA on x86_64 BIOS/UEFI, virtio-sound on
 * aarch64 UEFI, a no-op stub elsewhere) and the surface below stays
 * the same so scripts work portably.
 *
 * Format is fixed at:
 *   - PCM signed 16-bit little-endian
 *   - 2 channels (stereo) interleaved L, R, L, R, ...
 *   - 44.1 kHz sample rate
 *
 * Future revisions may add format negotiation; sticking to one
 * shape keeps the first audio milestone trivial to verify.
 */

#define HAL_AUDIO_CHANNELS 2u
#define HAL_AUDIO_RATE_HZ  44100u
#define HAL_AUDIO_BPS      2u  /* bytes per sample per channel */

/* Initialise whichever sound device the current build target ships.
 * Returns true if a real device was discovered and a stream allocated;
 * false means subsequent hal_audio_write / hal_audio_flush calls
 * silently drop samples. Safe to call twice; second call is no-op. */
bool hal_audio_init(void);

/* Push interleaved stereo s16le frames into the DMA ring. `frames` is
 * the number of L+R pairs, NOT the byte count. Returns the number of
 * frames actually accepted (may be 0 if the ring is full). */
uint32_t hal_audio_write(const int16_t *samples, uint32_t frames);

/* Wait for the device to drain everything currently queued. Useful
 * before issuing a stop to keep the tail samples from being clipped. */
void hal_audio_flush(void);

/* Stop the stream cleanly. Subsequent writes resume play from
 * silence. Idempotent. */
void hal_audio_stop(void);

/* True iff the current build has a real audio backend (HDA / virtio-
 * sound) and that backend successfully bound a device at init time.
 * The cando audio library uses this to decide whether to report
 * "muted" up to scripts. */
bool hal_audio_present(void);

/* Name of the bound device, for diagnostic prints. */
const char *hal_audio_device_name(void);

#endif
