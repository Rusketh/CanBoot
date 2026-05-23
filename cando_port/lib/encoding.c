/*
 * cando encoding modules - hex and base64 codecs, used most often
 * alongside crypto.* for human-readable hash/HMAC output.
 *
 *   hex.encode(s)        -> lowercase hex string
 *   hex.decode(s)        -> decoded bytes (null on parse error)
 *
 *   base64.encode(s)     -> standard base64 (with padding)
 *   base64.decode(s)     -> decoded bytes (null on parse error)
 *
 * Inputs and outputs are cando strings, which carry an explicit length
 * so binary payloads survive.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

/* ---- hex --------------------------------------------------------------- */

static const char hex_digits[] = "0123456789abcdef";

static int unhex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int h_encode(CandoVM *vm, int argc, CandoValue *args) {
    const char *src = libutil_arg_cstr_at(args, argc, 0);
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    size_t n = strlen(src);
    static char out[16384];
    if (n * 2 > sizeof(out) - 1) n = (sizeof(out) - 1) / 2;
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex_digits[(unsigned char)src[i] >> 4];
        out[i * 2 + 1] = hex_digits[(unsigned char)src[i] & 0xF];
    }
    out[n * 2] = '\0';
    CandoString *s = cando_string_new(out, (uint32_t)(n * 2));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int h_decode(CandoVM *vm, int argc, CandoValue *args) {
    const char *src = libutil_arg_cstr_at(args, argc, 0);
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    size_t n = strlen(src);
    if (n % 2 != 0) { cando_vm_push(vm, cando_null()); return 1; }
    static char out[8192];
    size_t out_n = n / 2;
    if (out_n > sizeof(out)) out_n = sizeof(out);
    for (size_t i = 0; i < out_n; i++) {
        int hi = unhex_nibble(src[i * 2]);
        int lo = unhex_nibble(src[i * 2 + 1]);
        if (hi < 0 || lo < 0) { cando_vm_push(vm, cando_null()); return 1; }
        out[i] = (char)((hi << 4) | lo);
    }
    CandoString *s = cando_string_new(out, (uint32_t)out_n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry hex_methods[] = {
    { "encode", h_encode },
    { "decode", h_decode },
};

void canboot_cando_open_hexlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, hex_methods,
                             sizeof(hex_methods) / sizeof(hex_methods[0]));
    cando_vm_set_global(vm, "hex", obj_val, true);
}

/* ---- base64 ------------------------------------------------------------ */

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b_encode(CandoVM *vm, int argc, CandoValue *args) {
    const char *src = libutil_arg_cstr_at(args, argc, 0);
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    size_t n = strlen(src);
    static char out[16384];
    size_t cap = sizeof(out) - 1;
    size_t out_n = 0;
    size_t i = 0;
    while (i + 3 <= n && out_n + 4 <= cap) {
        uint32_t v = ((uint32_t)(unsigned char)src[i]   << 16)
                   | ((uint32_t)(unsigned char)src[i+1] <<  8)
                   |  (uint32_t)(unsigned char)src[i+2];
        out[out_n++] = b64_alphabet[(v >> 18) & 0x3F];
        out[out_n++] = b64_alphabet[(v >> 12) & 0x3F];
        out[out_n++] = b64_alphabet[(v >>  6) & 0x3F];
        out[out_n++] = b64_alphabet[(v      ) & 0x3F];
        i += 3;
    }
    if (i + 1 == n && out_n + 4 <= cap) {
        uint32_t v = (uint32_t)(unsigned char)src[i] << 16;
        out[out_n++] = b64_alphabet[(v >> 18) & 0x3F];
        out[out_n++] = b64_alphabet[(v >> 12) & 0x3F];
        out[out_n++] = '=';
        out[out_n++] = '=';
    } else if (i + 2 == n && out_n + 4 <= cap) {
        uint32_t v = ((uint32_t)(unsigned char)src[i]   << 16)
                   | ((uint32_t)(unsigned char)src[i+1] <<  8);
        out[out_n++] = b64_alphabet[(v >> 18) & 0x3F];
        out[out_n++] = b64_alphabet[(v >> 12) & 0x3F];
        out[out_n++] = b64_alphabet[(v >>  6) & 0x3F];
        out[out_n++] = '=';
    }
    out[out_n] = '\0';
    CandoString *s = cando_string_new(out, (uint32_t)out_n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int b_decode(CandoVM *vm, int argc, CandoValue *args) {
    const char *src = libutil_arg_cstr_at(args, argc, 0);
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    size_t n = strlen(src);
    /* trim trailing '=' for length math but keep them for the loop. */
    if (n % 4 != 0) { cando_vm_push(vm, cando_null()); return 1; }
    static char out[12288];
    size_t out_n = 0;
    for (size_t i = 0; i < n; i += 4) {
        int a = b64_value(src[i]);
        int b = b64_value(src[i + 1]);
        int c = src[i + 2] == '=' ? -2 : b64_value(src[i + 2]);
        int d = src[i + 3] == '=' ? -2 : b64_value(src[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            cando_vm_push(vm, cando_null());
            return 1;
        }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12)
                   | ((uint32_t)(c < 0 ? 0 : c) << 6) | (uint32_t)(d < 0 ? 0 : d);
        if (out_n < sizeof(out)) out[out_n++] = (char)((v >> 16) & 0xFF);
        if (c >= 0 && out_n < sizeof(out)) out[out_n++] = (char)((v >> 8) & 0xFF);
        if (d >= 0 && out_n < sizeof(out)) out[out_n++] = (char)(v & 0xFF);
    }
    CandoString *s = cando_string_new(out, (uint32_t)out_n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry base64_methods[] = {
    { "encode", b_encode },
    { "decode", b_decode },
};

void canboot_cando_open_base64lib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, base64_methods,
                             sizeof(base64_methods) / sizeof(base64_methods[0]));
    cando_vm_set_global(vm, "base64", obj_val, true);
}
