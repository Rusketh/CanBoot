/*
 * minimp3 vendored shim. Includes minimp3.h with MINIMP3_IMPLEMENTATION
 * so the decoder body lands here. The cando audio library calls into
 * the two entry points below; the upstream API is preserved verbatim
 * for anyone reading the minimp3 docs.
 *
 * Output: signed-16 stereo PCM. minimp3 returns whichever channel
 * count + sample rate the source MP3 had; the audio lib's resampler
 * handles conversion to the HAL's fixed 44.1 kHz stereo format.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"

#define MP3_FRAME_SAMPLES MINIMP3_MAX_SAMPLES_PER_FRAME

int canboot_mp3_decode(const uint8_t *buf, int len,
                       int16_t **out_pcm, int *out_frames,
                       int *out_channels, int *out_hz) {
    static mp3dec_t g_mp3;
    mp3dec_init(&g_mp3);

    /* Grow-on-demand PCM buffer. Most boot chimes are <1 MB decoded. */
    size_t cap = 0;
    size_t used = 0;
    int16_t *pcm = NULL;
    int channels = 0;
    int rate     = 0;

    const uint8_t *src = buf;
    int remaining = len;
    int16_t frame[MP3_FRAME_SAMPLES];

    while (remaining > 0) {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(&g_mp3, src, remaining, frame, &info);
        if (info.frame_bytes <= 0) break;
        src       += info.frame_bytes;
        remaining -= info.frame_bytes;
        if (samples == 0) continue;
        if (channels == 0) {
            channels = info.channels;
            rate     = info.hz;
        }
        size_t add = (size_t)samples * (size_t)info.channels;
        if (used + add > cap) {
            size_t new_cap = cap ? cap * 2 : 0x10000;
            while (new_cap < used + add) new_cap *= 2;
            int16_t *grow = (int16_t *)realloc(pcm, new_cap * sizeof(int16_t));
            if (!grow) { free(pcm); return -1; }
            pcm = grow;
            cap = new_cap;
        }
        memcpy(pcm + used, frame, add * sizeof(int16_t));
        used += add;
    }
    if (!pcm || used == 0 || channels == 0) {
        free(pcm);
        return -1;
    }
    *out_pcm      = pcm;
    *out_frames   = (int)(used / (size_t)channels);
    *out_channels = channels;
    *out_hz       = rate;
    return 0;
}

void canboot_mp3_free(int16_t *pcm) {
    free(pcm);
}
