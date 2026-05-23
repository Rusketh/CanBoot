/*
 * cando random module - cryptographically strong PRNG backed by Mbed
 * TLS's CTR_DRBG, seeded once from our hardware-poll entropy source.
 *
 *   random.bytes(n)           -> string of n random bytes
 *   random.int(min, max)      -> integer in [min, max] inclusive
 *   random.float()            -> double in [0.0, 1.0)
 *   random.uuid()             -> RFC 4122 v4 string
 *   random.hex(n)             -> hex string of n random bytes
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static mbedtls_entropy_context  g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static int                      g_seeded;

static int seed(void) {
    if (g_seeded) return 0;
    mbedtls_entropy_init(&g_ent);
    mbedtls_ctr_drbg_init(&g_drbg);
    int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent,
                                   (const unsigned char *)"canboot-random", 14);
    if (rc != 0) return rc;
    g_seeded = 1;
    return 0;
}

static int draw(unsigned char *out, size_t n) {
    if (seed() != 0) return -1;
    while (n) {
        size_t chunk = n > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : n;
        if (mbedtls_ctr_drbg_random(&g_drbg, out, chunk) != 0) return -1;
        out += chunk;
        n   -= chunk;
    }
    return 0;
}

static int r_bytes(CandoVM *vm, int argc, CandoValue *args) {
    int n = (int)libutil_arg_num_at(args, argc, 0, 16);
    if (n < 0) n = 0;
    if (n > 4096) n = 4096;
    static unsigned char buf[4096];
    if (draw(buf, (size_t)n) != 0) { cando_vm_push(vm, cando_null()); return 1; }
    CandoString *s = cando_string_new((const char *)buf, (uint32_t)n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int r_int(CandoVM *vm, int argc, CandoValue *args) {
    int64_t lo = (int64_t)libutil_arg_num_at(args, argc, 0, 0);
    int64_t hi = (int64_t)libutil_arg_num_at(args, argc, 1, lo + 1);
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)(hi - lo) + 1ull;
    uint64_t r;
    if (draw((unsigned char *)&r, sizeof(r)) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    int64_t pick = lo + (int64_t)(r % span);
    cando_vm_push(vm, cando_number((f64)pick));
    return 1;
}

static int r_float(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    uint64_t r;
    if (draw((unsigned char *)&r, sizeof(r)) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    /* Map the high 53 bits into [0, 1). */
    f64 v = (f64)(r >> 11) / (f64)((uint64_t)1 << 53);
    cando_vm_push(vm, cando_number(v));
    return 1;
}

static const char hex_digits[] = "0123456789abcdef";

static int r_hex(CandoVM *vm, int argc, CandoValue *args) {
    int n = (int)libutil_arg_num_at(args, argc, 0, 16);
    if (n < 0) n = 0;
    if (n > 2048) n = 2048;
    static unsigned char raw[2048];
    static char out[4097];
    if (draw(raw, (size_t)n) != 0) { cando_vm_push(vm, cando_null()); return 1; }
    for (int i = 0; i < n; i++) {
        out[i * 2]     = hex_digits[raw[i] >> 4];
        out[i * 2 + 1] = hex_digits[raw[i] & 0xF];
    }
    out[n * 2] = '\0';
    CandoString *s = cando_string_new(out, (uint32_t)(n * 2));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int r_uuid(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    unsigned char b[16];
    if (draw(b, 16) != 0) { cando_vm_push(vm, cando_null()); return 1; }
    /* RFC 4122 v4 bit-twiddling. */
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    char out[37];
    int o = 0;
    static const int dash_after[] = {4, 6, 8, 10};
    int g = 0;
    for (int i = 0; i < 16; i++) {
        out[o++] = hex_digits[b[i] >> 4];
        out[o++] = hex_digits[b[i] & 0xF];
        if (g < 4 && (i + 1) == dash_after[g]) {
            out[o++] = '-';
            g++;
        }
    }
    out[o] = '\0';
    CandoString *s = cando_string_new(out, (uint32_t)o);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry random_methods[] = {
    { "bytes", r_bytes },
    { "int",   r_int   },
    { "float", r_float },
    { "hex",   r_hex   },
    { "uuid",  r_uuid  },
};

void canboot_cando_open_randomlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, random_methods,
                             sizeof(random_methods) / sizeof(random_methods[0]));
    cando_vm_set_global(vm, "random", obj_val, true);
}
