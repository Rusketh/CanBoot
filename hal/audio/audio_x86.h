#ifndef CANBOOT_HAL_AUDIO_X86_H
#define CANBOOT_HAL_AUDIO_X86_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Private x86_64 audio backend interface. Each backend (Intel HDA, AC'97)
 * exposes this shape; hal/audio/audio_x86.c probes them in order and binds
 * the first present device to the public hal_audio_* surface. Mirrors the
 * pluggable NIC layer.
 */

bool        canboot_hda_init(void);
bool        canboot_hda_present(void);
const char *canboot_hda_device_name(void);
uint32_t    canboot_hda_write(const int16_t *samples, uint32_t frames);
void        canboot_hda_flush(void);
void        canboot_hda_stop(void);

bool        canboot_ac97_init(void);
bool        canboot_ac97_present(void);
const char *canboot_ac97_device_name(void);
uint32_t    canboot_ac97_write(const int16_t *samples, uint32_t frames);
void        canboot_ac97_flush(void);
void        canboot_ac97_stop(void);

#endif /* CANBOOT_HAL_AUDIO_X86_H */
