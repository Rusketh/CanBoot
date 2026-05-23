/*
 * cando audio module - LOVE 2D-shaped Source / mixer API.
 *
 * Surface (mirrors love.audio + Source: methods, flattened to
 * function-call style because cando doesn't bind methods to
 * returned handles):
 *
 *   src = audio.newSource(bytes)        decode WAV or MP3 -> Source
 *   audio.play   (src)                  start
 *   audio.stop   (src)                  stop + rewind
 *   audio.pause  (src)                  pause, keep position
 *   audio.resume (src)                  resume from paused
 *   audio.isPlaying(src)        -> bool
 *   audio.isPaused (src)        -> bool
 *   audio.setVolume(src, v)             0..1 per-source gain
 *   audio.getVolume(src)        -> num
 *   audio.setLooping(src, b)
 *   audio.isLooping(src)        -> bool
 *   audio.getDuration(src)      -> seconds
 *   audio.tell(src)             -> seconds (current playback time)
 *   audio.seek(src, t)                   t in seconds
 *   audio.release(src)                   drop the slot + free PCM
 *
 *   audio.setVolume(v)                   single-arg = master gain
 *   audio.getVolume()                    no arg = master gain
 *   audio.stop()                         no arg = stop ALL sources
 *   audio.pause()                        no arg = pause ALL sources
 *   audio.resume()                       no arg = resume ALL paused
 *
 *   audio.update([nframes])              explicit pump; called
 *                                        automatically from cando
 *                                        time.* / input.* hot paths
 *                                        so scripts that "just call
 *                                        play and move on" still
 *                                        get full playback.
 *
 *   audio.present()             -> bool  real HAL backend bound
 *   audio.deviceName()          -> str
 *   audio.activeCount()         -> num   sources currently playing
 *
 * The mixer is additive with saturation clamping. Per-source PCM
 * lives in a slot's malloc'd buffer at the HAL's fixed format
 * (44.1 kHz stereo s16); any other rate / channel count is
 * resampled at newSource() time so the per-frame mix stays a
 * pure integer multiply-and-accumulate.
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

/* minimp3 - decoder lives in cando_port/vendor_glue/minimp3/decoder.c. */
extern int  canboot_mp3_decode(const uint8_t *buf, int len,
                                int16_t **out_pcm, int *out_frames,
                                int *out_channels, int *out_hz);
extern void canboot_mp3_free(int16_t *pcm);

#define AUDIO_MAX_SOURCES 8u
#define MIX_CHUNK_FRAMES  2048u
#define AUDIO_PUMP_FRAMES 2048u       /* per auto-pump call */

#define READ16_LE(p) ((uint16_t)((p)[0]) | ((uint16_t)((p)[1]) << 8))
#define READ32_LE(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
                      ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))

struct audio_source {
    int       in_use;
    int       playing;
    int       paused;
    int       looping;
    int16_t  *pcm;          /* interleaved stereo s16 at 44.1 kHz */
    uint32_t  frames;       /* total frame count */
    uint32_t  pos;          /* current playback position (frames) */
    float     volume;       /* 0..1 */
};

static struct audio_source g_sources[AUDIO_MAX_SOURCES];
static float g_master_volume = 1.0f;

static int16_t *resample_to_hal(const int16_t *in, int in_frames,
                                int in_ch, int in_hz, int *out_frames);
static int decode_wav(const uint8_t *buf, int len,
                      int16_t **out_pcm, int *out_frames,
                      int *out_channels, int *out_hz);

/* ---- Decoder / resampler ------------------------------------------ */

static int16_t *resample_to_hal(const int16_t *in, int in_frames,
                                int in_ch, int in_hz, int *out_frames) {
    if (in_hz <= 0 || in_ch <= 0 || in_frames <= 0) return NULL;
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
        if (sz & 1) pos++;
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
        } else {
            sample = (int)src[0] | ((int)src[1] << 8) | ((int)src[2] << 16);
            if (sample & 0x800000) sample |= ~0xffffff;
            sample >>= 8;
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

/* Decode bytes to HAL-format interleaved stereo s16 44.1 kHz.
 * Frees intermediate buffers; caller frees the returned `*out_pcm`
 * with plain `free`. Returns 0 on success, -1 on failure. */
static int decode_to_hal(const uint8_t *buf, int len,
                         int16_t **out_pcm, uint32_t *out_frames) {
    int16_t *raw = NULL;
    int raw_frames = 0, raw_ch = 0, raw_hz = 0;
    int rc = -1;
    int is_mp3 = 0;

    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
        rc = decode_wav(buf, len, &raw, &raw_frames, &raw_ch, &raw_hz);
    } else if (len >= 3 && (memcmp(buf, "ID3", 3) == 0
                           || (buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0))) {
        rc = canboot_mp3_decode(buf, len, &raw, &raw_frames, &raw_ch, &raw_hz);
        is_mp3 = 1;
    }
    if (rc != 0 || !raw || raw_frames <= 0) {
        if (raw) (is_mp3 ? canboot_mp3_free(raw) : free(raw));
        return -1;
    }

    int hal_frames = 0;
    int16_t *hal_pcm = raw;
    if (raw_hz != (int)HAL_AUDIO_RATE_HZ || raw_ch != (int)HAL_AUDIO_CHANNELS) {
        hal_pcm = resample_to_hal(raw, raw_frames, raw_ch, raw_hz, &hal_frames);
        if (is_mp3) canboot_mp3_free(raw); else free(raw);
        if (!hal_pcm) return -1;
    } else {
        hal_frames = raw_frames;
        if (is_mp3) {
            /* minimp3's allocator path differs from plain malloc; copy
             * into a malloc-owned buffer so the caller can always
             * free() the result. */
            int16_t *copy = (int16_t *)malloc((size_t)hal_frames * 2 * sizeof(int16_t));
            if (!copy) { canboot_mp3_free(raw); return -1; }
            memcpy(copy, raw, (size_t)hal_frames * 2 * sizeof(int16_t));
            canboot_mp3_free(raw);
            hal_pcm = copy;
        }
    }
    *out_pcm    = hal_pcm;
    *out_frames = (uint32_t)hal_frames;
    return 0;
}

/* ---- Mixer pump --------------------------------------------------- */

void canboot_audio_pump(uint32_t max_frames) {
    if (max_frames == 0) return;

    /* Even when there's no HAL backend, advance source positions so
     * isPlaying() eventually returns false and the script doesn't
     * get a stuck "still playing" lie. */
    int have_backend = hal_audio_present();
    if (!have_backend) {
        for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
            struct audio_source *s = &g_sources[i];
            if (!s->in_use || !s->playing || s->paused) continue;
            uint32_t remaining = s->frames - s->pos;
            uint32_t step = remaining < max_frames ? remaining : max_frames;
            s->pos += step;
            if (s->pos >= s->frames) {
                if (s->looping) s->pos = 0;
                else s->playing = 0;
            }
        }
        return;
    }

    static int32_t mixbuf[MIX_CHUNK_FRAMES * 2];
    static int16_t outbuf[MIX_CHUNK_FRAMES * 2];

    while (max_frames > 0) {
        uint32_t chunk = max_frames > MIX_CHUNK_FRAMES ? MIX_CHUNK_FRAMES : max_frames;

        int any = 0;
        for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
            if (g_sources[i].in_use && g_sources[i].playing && !g_sources[i].paused) {
                any = 1; break;
            }
        }
        if (!any) return;

        for (uint32_t k = 0; k < chunk * 2; k++) mixbuf[k] = 0;

        for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
            struct audio_source *s = &g_sources[i];
            if (!s->in_use || !s->playing || s->paused) continue;
            int vol_q15 = (int)(s->volume * g_master_volume * 32768.0f + 0.5f);
            if (vol_q15 < 0) vol_q15 = 0;
            if (vol_q15 > 32768) vol_q15 = 32768;
            for (uint32_t f = 0; f < chunk; f++) {
                if (s->pos >= s->frames) {
                    if (s->looping) {
                        s->pos = 0;
                    } else {
                        s->playing = 0;
                        break;
                    }
                }
                int16_t l = s->pcm[s->pos * 2 + 0];
                int16_t r = s->pcm[s->pos * 2 + 1];
                mixbuf[f * 2 + 0] += ((int32_t)l * vol_q15) >> 15;
                mixbuf[f * 2 + 1] += ((int32_t)r * vol_q15) >> 15;
                s->pos++;
            }
        }

        for (uint32_t k = 0; k < chunk * 2; k++) {
            int32_t v = mixbuf[k];
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            outbuf[k] = (int16_t)v;
        }

        uint32_t pushed = hal_audio_write(outbuf, chunk);
        if (pushed == 0) return;  /* ring full; drop the rest */
        max_frames -= pushed;
        if (pushed < chunk) {
            /* HAL couldn't take the full chunk - back off and let
             * the next pump finish the job. */
            return;
        }
    }
}

/* Convenience wrapper called by other cando libs so audio just
 * works without scripts having to remember audio.update(). */
void canboot_audio_pump_default(void) {
    canboot_audio_pump(AUDIO_PUMP_FRAMES);
}

/* ---- Source helpers ----------------------------------------------- */

static int alloc_source(void) {
    for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
        if (!g_sources[i].in_use) return (int)i;
    }
    return -1;
}

static struct audio_source *get_source(int h) {
    if (h < 0 || h >= (int)AUDIO_MAX_SOURCES) return NULL;
    if (!g_sources[h].in_use) return NULL;
    return &g_sources[h];
}

/* ---- Cando bindings ----------------------------------------------- */

static int f_new_source(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *str = libutil_arg_str_at(args, argc, 0);
    if (!str || str->length < 4) {
        cando_vm_push(vm, cando_number(-1));
        return 1;
    }
    int16_t *pcm = NULL;
    uint32_t frames = 0;
    if (decode_to_hal((const uint8_t *)str->data, (int)str->length,
                      &pcm, &frames) != 0) {
        cando_vm_push(vm, cando_number(-1));
        return 1;
    }
    int h = alloc_source();
    if (h < 0) {
        free(pcm);
        cando_vm_push(vm, cando_number(-1));
        return 1;
    }
    struct audio_source *s = &g_sources[h];
    memset(s, 0, sizeof(*s));
    s->in_use  = 1;
    s->pcm     = pcm;
    s->frames  = frames;
    s->volume  = 1.0f;
    cando_vm_push(vm, cando_number((double)h));
    return 1;
}

static int f_play(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    s->playing = 1;
    s->paused  = 0;
    /* Kick the mixer so short clips don't need a follow-up call to
     * get any samples into the HAL ring. */
    canboot_audio_pump(AUDIO_PUMP_FRAMES);
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_stop(CandoVM *vm, int argc, CandoValue *args) {
    if (argc == 0) {
        /* Global: stop everything + rewind. */
        for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
            if (!g_sources[i].in_use) continue;
            g_sources[i].playing = 0;
            g_sources[i].paused  = 0;
            g_sources[i].pos     = 0;
        }
        cando_vm_push(vm, cando_bool(1));
        return 1;
    }
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    s->playing = 0;
    s->paused  = 0;
    s->pos     = 0;
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_pause(CandoVM *vm, int argc, CandoValue *args) {
    if (argc == 0) {
        for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
            if (g_sources[i].in_use && g_sources[i].playing) {
                g_sources[i].paused = 1;
            }
        }
        cando_vm_push(vm, cando_bool(1));
        return 1;
    }
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    s->paused = 1;
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_resume(CandoVM *vm, int argc, CandoValue *args) {
    if (argc == 0) {
        for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
            if (g_sources[i].in_use && g_sources[i].paused) {
                g_sources[i].paused = 0;
            }
        }
        canboot_audio_pump(AUDIO_PUMP_FRAMES);
        cando_vm_push(vm, cando_bool(1));
        return 1;
    }
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    s->paused = 0;
    canboot_audio_pump(AUDIO_PUMP_FRAMES);
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_is_playing(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    cando_vm_push(vm, cando_bool(s && s->playing && !s->paused));
    return 1;
}

static int f_is_paused(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    cando_vm_push(vm, cando_bool(s && s->paused));
    return 1;
}

static int f_set_volume(CandoVM *vm, int argc, CandoValue *args) {
    /* Overload: single-arg sets master volume, two-arg sets per-source. */
    if (argc == 1) {
        double v = libutil_arg_num_at(args, argc, 0, 1.0);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        g_master_volume = (float)v;
        cando_vm_push(vm, cando_bool(1));
        return 1;
    }
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    double v = libutil_arg_num_at(args, argc, 1, 1.0);
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    s->volume = (float)v;
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_get_volume(CandoVM *vm, int argc, CandoValue *args) {
    if (argc == 0) {
        cando_vm_push(vm, cando_number((double)g_master_volume));
        return 1;
    }
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    cando_vm_push(vm, cando_number(s ? (double)s->volume : 0.0));
    return 1;
}

static int f_set_looping(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    int b = (int)libutil_arg_num_at(args, argc, 1, 0);
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    s->looping = b ? 1 : 0;
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_is_looping(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    cando_vm_push(vm, cando_bool(s && s->looping));
    return 1;
}

static int f_get_duration(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    cando_vm_push(vm, cando_number(s ? (double)s->frames / (double)HAL_AUDIO_RATE_HZ : 0.0));
    return 1;
}

static int f_tell(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    cando_vm_push(vm, cando_number(s ? (double)s->pos / (double)HAL_AUDIO_RATE_HZ : 0.0));
    return 1;
}

static int f_seek(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    double t = libutil_arg_num_at(args, argc, 1, 0.0);
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    if (t < 0.0) t = 0.0;
    uint64_t frame = (uint64_t)(t * (double)HAL_AUDIO_RATE_HZ);
    if (frame > s->frames) frame = s->frames;
    s->pos = (uint32_t)frame;
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_release(CandoVM *vm, int argc, CandoValue *args) {
    int h = (int)libutil_arg_num_at(args, argc, 0, -1);
    struct audio_source *s = get_source(h);
    if (!s) { cando_vm_push(vm, cando_bool(0)); return 1; }
    free(s->pcm);
    memset(s, 0, sizeof(*s));
    cando_vm_push(vm, cando_bool(1));
    return 1;
}

static int f_update(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t n = (uint32_t)libutil_arg_num_at(args, argc, 0, (double)AUDIO_PUMP_FRAMES);
    canboot_audio_pump(n);
    cando_vm_push(vm, cando_bool(1));
    return 1;
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

static int f_active_count(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    int n = 0;
    for (uint32_t i = 0; i < AUDIO_MAX_SOURCES; i++) {
        if (g_sources[i].in_use && g_sources[i].playing && !g_sources[i].paused) {
            n++;
        }
    }
    cando_vm_push(vm, cando_number((double)n));
    return 1;
}

static const LibutilMethodEntry audio_methods[] = {
    { "newSource",   f_new_source   },
    { "play",        f_play         },
    { "stop",        f_stop         },
    { "pause",       f_pause        },
    { "resume",      f_resume       },
    { "isPlaying",   f_is_playing   },
    { "isPaused",    f_is_paused    },
    { "setVolume",   f_set_volume   },
    { "getVolume",   f_get_volume   },
    { "setLooping",  f_set_looping  },
    { "isLooping",   f_is_looping   },
    { "getDuration", f_get_duration },
    { "tell",        f_tell         },
    { "seek",        f_seek         },
    { "release",     f_release      },
    { "update",      f_update       },
    { "present",     f_present      },
    { "deviceName",  f_device_name  },
    { "activeCount", f_active_count },
};

void canboot_cando_open_audiolib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, audio_methods,
                             sizeof(audio_methods) / sizeof(audio_methods[0]));
    cando_vm_set_global(vm, "audio", obj_val, true);
}
