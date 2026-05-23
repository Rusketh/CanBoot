/*
 * cando crypto module - hash + HMAC primitives over Mbed TLS.
 *
 *   crypto.sha256(data)             -> 32-byte digest as string
 *   crypto.sha512(data)             -> 64-byte digest as string
 *   crypto.sha256Hex(data)          -> 64-char lowercase hex digest
 *   crypto.sha512Hex(data)          -> 128-char lowercase hex digest
 *   crypto.hmacSha256(key, data)    -> 32-byte HMAC as string
 *   crypto.hmacSha256Hex(key, data) -> 64-char hex HMAC
 *
 * All inputs and outputs are cando strings (raw bytes are fine - cando
 * strings carry a length so binary content survives).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static const char hex_digits[] = "0123456789abcdef";

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex_digits[in[i] >> 4];
        out[i * 2 + 1] = hex_digits[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static const char *arg_data(CandoValue *args, int argc, int idx, size_t *out_len) {
    const char *s = libutil_arg_cstr_at(args, argc, idx);
    *out_len = s ? strlen(s) : 0;
    return s;
}

static int c_sha256(CandoVM *vm, int argc, CandoValue *args) {
    size_t n; const char *data = arg_data(args, argc, 0, &n);
    if (!data) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char *)data, n, digest, 0) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new((const char *)digest, 32);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int c_sha512(CandoVM *vm, int argc, CandoValue *args) {
    size_t n; const char *data = arg_data(args, argc, 0, &n);
    if (!data) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char digest[64];
    if (mbedtls_sha512((const unsigned char *)data, n, digest, 0) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new((const char *)digest, 64);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int c_sha256_hex(CandoVM *vm, int argc, CandoValue *args) {
    size_t n; const char *data = arg_data(args, argc, 0, &n);
    if (!data) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char *)data, n, digest, 0) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    char out[65];
    hex_encode(digest, 32, out);
    CandoString *s = cando_string_new(out, 64);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int c_sha512_hex(CandoVM *vm, int argc, CandoValue *args) {
    size_t n; const char *data = arg_data(args, argc, 0, &n);
    if (!data) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char digest[64];
    if (mbedtls_sha512((const unsigned char *)data, n, digest, 0) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    char out[129];
    hex_encode(digest, 64, out);
    CandoString *s = cando_string_new(out, 128);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int hmac_compute(mbedtls_md_type_t algo,
                        const unsigned char *key, size_t kn,
                        const unsigned char *data, size_t dn,
                        unsigned char *out) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(algo);
    if (!info) return -1;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, key, kn);
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, data, dn);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return rc;
}

static int c_hmac_sha256(CandoVM *vm, int argc, CandoValue *args) {
    size_t kn; const char *key  = arg_data(args, argc, 0, &kn);
    size_t dn; const char *data = arg_data(args, argc, 1, &dn);
    if (!key || !data) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char digest[32];
    if (hmac_compute(MBEDTLS_MD_SHA256,
                     (const unsigned char *)key, kn,
                     (const unsigned char *)data, dn, digest) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new((const char *)digest, 32);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int c_hmac_sha256_hex(CandoVM *vm, int argc, CandoValue *args) {
    size_t kn; const char *key  = arg_data(args, argc, 0, &kn);
    size_t dn; const char *data = arg_data(args, argc, 1, &dn);
    if (!key || !data) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char digest[32];
    if (hmac_compute(MBEDTLS_MD_SHA256,
                     (const unsigned char *)key, kn,
                     (const unsigned char *)data, dn, digest) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    char out[65];
    hex_encode(digest, 32, out);
    CandoString *s = cando_string_new(out, 64);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry crypto_methods[] = {
    { "sha256",         c_sha256         },
    { "sha512",         c_sha512         },
    { "sha256Hex",      c_sha256_hex     },
    { "sha512Hex",      c_sha512_hex     },
    { "hmacSha256",     c_hmac_sha256    },
    { "hmacSha256Hex",  c_hmac_sha256_hex },
};

void canboot_cando_open_cryptolib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, crypto_methods,
                             sizeof(crypto_methods) / sizeof(crypto_methods[0]));
    cando_vm_set_global(vm, "crypto", obj_val, true);
}
