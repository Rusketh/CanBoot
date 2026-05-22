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

static const LibutilMethodEntry fmt_methods[] = {
    { "sprintf", f_sprintf },
};

void canboot_cando_open_fmtlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, fmt_methods,
                             sizeof(fmt_methods) / sizeof(fmt_methods[0]));
    cando_vm_set_global(vm, "fmt", obj_val, true);
}
