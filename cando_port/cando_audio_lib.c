/*
 * cando audio module - decode and play WAV / MP3 through the HAL
 * audio surface. Decoding lives here; the actual sample push goes
 * through hal_audio_write at the fixed 44.1 kHz stereo s16le format
 * the HAL exposes. A muted / absent backend silently drops samples
 * but `audio.present()` lets scripts check first.
 *
 *   audio.present()                bool - real device bound
 *   audio.deviceName()             "intel-hda" / "virtio-snd" / "none"
 *   audio.play(bytes)              autodetect WAV / MP3 + queue for play
 *   audio.stop()                   stop and drain
 *
 * WAV parsing handles standard PCM (format=1) with 8/16/24-bit
 * depths and 1 or 2 channels at any sample rate; non-44.1 kHz input
 * gets nearest-neighbour resampled before push (good enough for
 * boot chimes; a proper resampler can land later).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

#include "hal/audio.h"

/* minimp3 - decoder lives in cando_port/minimp3_canboot.c. */
struct mp3dec;
struct mp3dec_frame_info;
extern int canboot_mp3_decode(const uint8_t *buf, int len,
                              int16_t **out_pcm, int *out_frames,
                              int *out_channels, int *out_hz);
extern void canboot_mp3_free(int16_t *pcm);

#define READ16_LE(p) ((uint16_t)((p)[0]) | ((uint16_t)((p)[1]) << 8))
#define READ32_LE(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
                      ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))

/* Convert input PCM (channels in_ch, rate in_hz, 16-bit signed)
 * into 44.1 kHz stereo s16le suitable for hal_audio_write. Returns
 * a freshly allocated buffer (caller frees) and the frame count
 * in *out_frames. */
static int16_t *resample_to_hal(const int16_t *in, int in_frames,
                                int in_ch, int in_hz,
                                int *out_frames) {
    if (in_hz <= 0 || in_ch <= 0) return NULL;
    double ratio = (double)HAL_AUDIO_RATE_HZ / (double)in_hz;
    int out_n = (int)((double)in_frames * ratio);
    if (out_n <= 0) return NULL;
    int16_t *out = (int16_t *)malloc((size_t)out_n * 2 * sizeof(int16_t));
    if (!out) return NULL;
    for (int i = 0; i < out_n; i++) {
        int src_i = (int)((double)i / ratio);
        if (src_i >= in_frames) src_i = in_frames - 1;
        const int16_t *src = in + (size_t)src_i * in_ch;
        int16_t l = src[0];
        int16_t r = (in_ch >= 2) ? src[1] : src[0];
        out[i * 2 + 0] = l;
        out[i * 2 + 1] = r;
    }
    *out_frames = out_n;
    return out;
}

/* Decode a RIFF/WAVE PCM stream into a freshly-allocated 16-bit
 * signed buffer (interleaved). Returns 0 on success and fills out
 * the channel count, sample rate, and frame count. Supports
 * 8/16/24-bit PCM (format=1) only; anything else returns -1. */
static int decode_wav(const uint8_t *buf, int len,
                      int16_t **out_pcm, int *out_frames,
                      int *out_channels, int *out_hz) {
    if (len < 44) return -1;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return -1;

    int pos = 12;
    int fmt_off = -1, fmt_size = 0;
    int data_off = -1, data_size = 0;
    while (pos + 8 <= len) {
        const uint8_t *tag = buf + pos;
        uint32_t sz = READ32_LE(buf + pos + 4);
        pos += 8;
        if (memcmp(tag, "fmt ", 4) == 0) {
            fmt_off = pos; fmt_size = (int)sz;
        } else if (memcmp(tag, "data", 4) == 0) {
            data_off = pos; data_size = (int)sz;
            break;
        }
        pos += (int)sz;
        if (sz & 1) pos++; /* chunk alignment */
    }
    if (fmt_off < 0 || data_off < 0 || fmt_size < 16) return -1;
    if (data_off + data_size > len) data_size = len - data_off;

    uint16_t format    = READ16_LE(buf + fmt_off + 0);
    uint16_t channels  = READ16_LE(buf + fmt_off + 2);
    uint32_t rate      = READ32_LE(buf + fmt_off + 4);
    uint16_t bps       = READ16_LE(buf + fmt_off + 14);
    if (format != 1) return -1;
    if (channels != 1 && channels != 2) return -1;
    if (bps != 8 && bps != 16 && bps != 24) return -1;

    int bytes_per_frame = channels * (bps / 8);
    int frames = data_size / bytes_per_frame;
    int16_t *pcm = (int16_t *)malloc((size_t)frames * channels * sizeof(int16_t));
    if (!pcm) return -1;
    const uint8_t *src = buf + data_off;
    for (int i = 0; i < frames * channels; i++) {
        int sample = 0;
        if (bps == 8) {
            sample = ((int)src[0] - 128) * 256;
            src += 1;
        } else if (bps == 16) {
            sample = (int16_t)READ16_LE(src);
            src += 2;
        } else { /* 24-bit */
            sample = (int)src[0] | ((int)src[1] << 8) | ((int)src[2] << 16);
            if (sample & 0x800000) sample |= ~0xffffff;
            sample >>= 8; /* down to 16-bit */
            src += 3;
        }
        if (sample >  32767) sample =  32767;
        if (sample < -32768) sample = -32768;
        pcm[i] = (int16_t)sample;
    }
    *out_pcm      = pcm;
    *out_frames   = frames;
    *out_channels = channels;
    *out_hz       = (int)rate;
    return 0;
}

static int f_present(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_bool(hal_audio_present()));
    return 1;
}

static int f_device_name(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    const char *name = hal_audio_device_name();
    CandoString *s = cando_string_new(name, (uint32_t)strlen(name));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int f_stop(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    hal_audio_stop();
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int f_play(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *str = libutil_arg_str_at(args, argc, 0);
    if (!str || str->length < 4) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    const uint8_t *buf = (const uint8_t *)str->data;
    int len = (int)str->length;
    int16_t *pcm = NULL;
    int frames = 0, channels = 0, hz = 0;
    int decoded = -1;
    bool is_mp3 = false;

    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
        decoded = decode_wav(buf, len, &pcm, &frames, &channels, &hz);
    } else if (len >= 3 && (memcmp(buf, "ID3", 3) == 0 || (buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0))) {
        decoded = canboot_mp3_decode(buf, len, &pcm, &frames, &channels, &hz);
        is_mp3 = true;
    }

    if (decoded != 0 || !pcm || frames <= 0) {
        if (pcm) (is_mp3 ? canboot_mp3_free(pcm) : free(pcm));
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }

    /* Resample to 44.1 kHz stereo if needed. */
    int hal_frames = 0;
    int16_t *hal_pcm = pcm;
    if (hz != (int)HAL_AUDIO_RATE_HZ || channels != (int)HAL_AUDIO_CHANNELS) {
        hal_pcm = resample_to_hal(pcm, frames, channels, hz, &hal_frames);
        (is_mp3 ? canboot_mp3_free(pcm) : free(pcm));
        if (!hal_pcm) {
            cando_vm_push(vm, cando_bool(false));
            return 1;
        }
    } else {
        hal_frames = frames;
    }

    /* Push to HAL. Loop because the ring may be smaller than the
     * payload; hal_audio_write returns the count actually accepted. */
    uint32_t pushed = 0;
    while (pushed < (uint32_t)hal_frames) {
        uint32_t n = hal_audio_write(hal_pcm + pushed * 2, hal_frames - pushed);
        if (n == 0) break;
        pushed += n;
    }
    hal_audio_flush();
    free(hal_pcm);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static const LibutilMethodEntry audio_methods[] = {
    { "present",    f_present     },
    { "deviceName", f_device_name },
    { "play",       f_play        },
    { "stop",       f_stop        },
};

void canboot_cando_open_audiolib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, audio_methods,
                             sizeof(audio_methods) / sizeof(audio_methods[0]));
    cando_vm_set_global(vm, "audio", obj_val, true);
}
