/*
 * Fallback audio HAL backend. Used when the build target has no real
 * sound device (current state for everything pre-HDA / pre-virtio-
 * sound), or when device probing fails. Samples passed to write are
 * dropped on the floor but the writer's frame count is honoured so
 * scripts don't deadlock waiting for the ring to drain.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/audio.h"

__attribute__((weak))
bool hal_audio_init(void) {
    return false;
}

__attribute__((weak))
uint32_t hal_audio_write(const int16_t *samples, uint32_t frames) {
    (void)samples;
    return frames;
}

__attribute__((weak))
void hal_audio_flush(void) { }

__attribute__((weak))
void hal_audio_stop(void) { }

__attribute__((weak))
bool hal_audio_present(void) {
    return false;
}

__attribute__((weak))
const char *hal_audio_device_name(void) {
    return "none";
}
