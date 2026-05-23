/*
 * cando fmt module - minimal printf-style formatter that takes a
 * single format string and a variadic-ish call shape mapping
 * arguments to the order they appear.
 *
 *   fmt.sprintf("addr=%s port=%d", host, port)
 *
 * Supported conversions: %s (cstr), %d (int from cando_number), %x
 * (hex int), %f (double). The %f formatter uses snprintf so cando-
 * side fractional output is whatever picolibc emits.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static int f_sprintf(CandoVM *vm, int argc, CandoValue *args) {
    const char *fmt = libutil_arg_cstr_at(args, argc, 0);
    if (!fmt) { cando_vm_push(vm, cando_null()); return 1; }

    static char out[4096];
    size_t o = 0;
    int arg_i = 1;
    const char *p = fmt;
    while (*p && o < sizeof(out) - 1) {
        if (*p != '%') { out[o++] = *p++; continue; }
        p++;
        char conv = *p ? *p++ : 0;
        if (!conv) break;
        char tmp[64]; int n = 0;
        switch (conv) {
            case '%': out[o++] = '%'; continue;
            case 's': {
                const char *s = libutil_arg_cstr_at(args, argc, arg_i++);
                if (s) {
                    while (*s && o < sizeof(out) - 1) out[o++] = *s++;
                }
                continue;
            }
            case 'd':
            case 'i': {
                int64_t v = (int64_t)libutil_arg_num_at(args, argc, arg_i++, 0);
                n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
                break;
            }
            case 'u': {
                uint64_t v = (uint64_t)libutil_arg_num_at(args, argc, arg_i++, 0);
                n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
                break;
            }
            case 'x': {
                uint64_t v = (uint64_t)libutil_arg_num_at(args, argc, arg_i++, 0);
                n = snprintf(tmp, sizeof(tmp), "%llx", (unsigned long long)v);
                break;
            }
            case 'X': {
                uint64_t v = (uint64_t)libutil_arg_num_at(args, argc, arg_i++, 0);
                n = snprintf(tmp, sizeof(tmp), "%llX", (unsigned long long)v);
                break;
            }
            case 'f': {
                double v = (double)libutil_arg_num_at(args, argc, arg_i++, 0);
                n = snprintf(tmp, sizeof(tmp), "%f", v);
                break;
            }
            default:
                tmp[0] = '%'; tmp[1] = conv; tmp[2] = '\0'; n = 2;
                break;
        }
        if (n < 0) n = 0;
        for (int k = 0; k < n && o < sizeof(out) - 1; k++) out[o++] = tmp[k];
    }
    out[o] = '\0';
    CandoString *s = cando_string_new(out, (uint32_t)o);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* Pack a 16-bit unsigned int as 2 raw little-endian bytes. Used by
 * scripts that build binary file headers (WAV, BMP, etc.) without
 * having to fall back to a base-16/base-64 round-trip. */
static int f_u16le(CandoVM *vm, int argc, CandoValue *args) {
    uint16_t v = (uint16_t)(uint64_t)libutil_arg_num_at(args, argc, 0, 0);
    char buf[2];
    buf[0] = (char)(v & 0xff);
    buf[1] = (char)((v >> 8) & 0xff);
    CandoString *s = cando_string_new(buf, 2);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int f_u32le(CandoVM *vm, int argc, CandoValue *args) {
    uint32_t v = (uint32_t)(uint64_t)libutil_arg_num_at(args, argc, 0, 0);
    char buf[4];
    buf[0] = (char)(v & 0xff);
    buf[1] = (char)((v >> 8) & 0xff);
    buf[2] = (char)((v >> 16) & 0xff);
    buf[3] = (char)((v >> 24) & 0xff);
    CandoString *s = cando_string_new(buf, 4);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* Generate a `n_samples` long 16-bit signed PCM sine wave at `freq`
 * Hz against sample rate `sr`. Returned as raw little-endian bytes
 * so scripts can splice it straight into a RIFF/WAVE body. */
static int f_sine_wave_16(CandoVM *vm, int argc, CandoValue *args) {
    int freq = (int)libutil_arg_num_at(args, argc, 0, 440);
    int sr   = (int)libutil_arg_num_at(args, argc, 1, 44100);
    int n    = (int)libutil_arg_num_at(args, argc, 2, 11025);
    if (n <= 0 || sr <= 0) {
        CandoString *empty = cando_string_new("", 0);
        cando_vm_push(vm, cando_string_value(empty));
        return 1;
    }
    char *buf = (char *)malloc((size_t)n * 2);
    if (!buf) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    const double TAU = 6.28318530717958647692;
    for (int i = 0; i < n; i++) {
        double t = (double)i / (double)sr;
        double s = sin(TAU * (double)freq * t);
        int16_t v = (int16_t)(s * 30000.0);
        buf[i * 2]     = (char)(v & 0xff);
        buf[i * 2 + 1] = (char)((v >> 8) & 0xff);
    }
    CandoString *s = cando_string_new(buf, (uint32_t)(n * 2));
    free(buf);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry fmt_methods[] = {
    { "sprintf",    f_sprintf       },
    { "u16le",      f_u16le         },
    { "u32le",      f_u32le         },
    { "sineWave16", f_sine_wave_16  },
};

void canboot_cando_open_fmtlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, fmt_methods,
                             sizeof(fmt_methods) / sizeof(fmt_methods[0]));
    cando_vm_set_global(vm, "fmt", obj_val, true);
}
