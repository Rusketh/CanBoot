/*
 * x86_64 audio dispatcher. Probes the available output backends in order
 * (Intel HDA, then AC'97) and binds the first present device to the public
 * hal_audio_* surface, routing every call to it. This is the strong
 * hal_audio_* provider on x86_64 (overriding the weak stub); the per-device
 * drivers expose canboot_hda_* / canboot_ac97_* instead.
 */

#if defined(__x86_64__)

#include <stdbool.h>
#include <stdint.h>

#include "hal/audio.h"
#include "audio_x86.h"

enum backend { BK_NONE = 0, BK_HDA, BK_AC97 };
static enum backend g_bk;

bool hal_audio_init(void) {
    if (g_bk != BK_NONE) return true;
    if (canboot_hda_init())  { g_bk = BK_HDA;  return true; }
    if (canboot_ac97_init()) { g_bk = BK_AC97; return true; }
    return false;
}

bool hal_audio_present(void) {
    return g_bk != BK_NONE;
}

const char *hal_audio_device_name(void) {
    switch (g_bk) {
        case BK_HDA:  return canboot_hda_device_name();
        case BK_AC97: return canboot_ac97_device_name();
        default:      return "none";
    }
}

uint32_t hal_audio_write(const int16_t *samples, uint32_t frames) {
    switch (g_bk) {
        case BK_HDA:  return canboot_hda_write(samples, frames);
        case BK_AC97: return canboot_ac97_write(samples, frames);
        default:      return frames;   /* drop, honour the count */
    }
}

void hal_audio_flush(void) {
    switch (g_bk) {
        case BK_HDA:  canboot_hda_flush();  break;
        case BK_AC97: canboot_ac97_flush(); break;
        default: break;
    }
}

void hal_audio_stop(void) {
    switch (g_bk) {
        case BK_HDA:  canboot_hda_stop();  break;
        case BK_AC97: canboot_ac97_stop(); break;
        default: break;
    }
}

#endif /* __x86_64__ */
