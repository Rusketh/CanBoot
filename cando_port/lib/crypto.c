/*
 * cando crypto module — drop-in for CanDo's `crypto` surface,
 * Mbed-TLS-backed, scoped to what the canboot bare-metal build can
 * support today.
 *
 * Drop-in shape (matches vendor/cando/source/lib/crypto.c):
 *   crypto.md5(data, enc?)         -> string
 *   crypto.sha1(data, enc?)        -> string
 *   crypto.sha224(data, enc?)      -> string
 *   crypto.sha256(data, enc?)      -> string  (default enc "hex")
 *   crypto.sha384(data, enc?)      -> string
 *   crypto.sha512(data, enc?)      -> string
 *   crypto.hash(algo, data, enc?)  -> string  (dynamic algo dispatch)
 *   crypto.hmac(algo, key, data, enc?) -> string
 *
 *   crypto.hex.encode(bytes)       -> string  (lowercase hex)
 *   crypto.hex.decode(hex)         -> string  (binary)
 *
 *   crypto.timingSafeEqual(a, b)   -> bool
 *
 * Bare-metal limitations vs host CanDo:
 *   - SHA3 / BLAKE2 / scrypt / Argon2 / RSA / EC / Ed25519 / X.509 /
 *     AES-GCM / DH / publicEncrypt / generateKeyPair land in
 *     follow-up commits as Mbed TLS bindings are wired.
 *   - Encoding param "base64" / "base64url" pending a base64 helper
 *     (use crypto.hex for now or wrap the bytes via `encoding.*`).
 *
 * Legacy aliases (canboot-specific, kept for current callers + smoke
 * test continuity):
 *   crypto.sha256Hex(data)         -> always hex
 *   crypto.sha512Hex(data)         -> always hex
 *   crypto.hmacSha256(key, data)   -> raw 32 bytes
 *   crypto.hmacSha256Hex(key, data) -> always hex
 *
 * The drop-in functions follow CanDo's throw-on-bad-input convention;
 * benign cases (unknown algo, malformed enc) throw via canboot_error_throw.
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

#include "error.h"

/* ---- Hex helpers ----------------------------------------------------- */

static const char hex_digits[] = "0123456789abcdef";

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex_digits[in[i] >> 4];
        out[i * 2 + 1] = hex_digits[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static int hex_decode_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns number of bytes written, or -1 on parse failure. */
static int hex_decode(const char *in, size_t len, unsigned char *out, size_t cap) {
    if (len & 1) return -1;
    if (len / 2 > cap) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hex_decode_nibble((unsigned char)in[i * 2]);
        int lo = hex_decode_nibble((unsigned char)in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return (int)(len / 2);
}

/* ---- Algorithm resolution + encoding selector ----------------------- */

/* Map CanDo's algorithm-name string to an Mbed TLS digest type.
 * Returns MBEDTLS_MD_NONE for unsupported names. */
static mbedtls_md_type_t resolve_md(const char *name) {
    if (!name) return MBEDTLS_MD_NONE;
    if (strcmp(name, "md5")     == 0) return MBEDTLS_MD_MD5;
    if (strcmp(name, "sha1")    == 0) return MBEDTLS_MD_SHA1;
    if (strcmp(name, "sha224")  == 0) return MBEDTLS_MD_SHA224;
    if (strcmp(name, "sha256")  == 0) return MBEDTLS_MD_SHA256;
    if (strcmp(name, "sha384")  == 0) return MBEDTLS_MD_SHA384;
    if (strcmp(name, "sha512")  == 0) return MBEDTLS_MD_SHA512;
    return MBEDTLS_MD_NONE;
}

typedef enum {
    ENC_HEX,
    ENC_BYTES,
    ENC_BAD,
} CryptoEnc;

static CryptoEnc enc_from_arg(CandoValue *args, int argc, int idx) {
    const char *s = libutil_arg_cstr_at(args, argc, idx);
    if (!s)                          return ENC_HEX;     /* CanDo default */
    if (strcmp(s, "hex")    == 0)    return ENC_HEX;
    if (strcmp(s, "bytes")  == 0)    return ENC_BYTES;
    if (strcmp(s, "binary") == 0)    return ENC_BYTES;
    /* "base64" / "base64url" land in a follow-up. */
    return ENC_BAD;
}

/* Push a digest in the requested encoding. Returns 1 (one value pushed)
 * on success; throws via canboot_error_throw on bad input. */
static int push_digest(CandoVM *vm,
                       const unsigned char *digest, size_t dn,
                       CryptoEnc enc, const char *for_call)
{
    if (enc == ENC_BAD) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: unsupported encoding (use 'hex' or 'bytes')", for_call);
    }
    if (enc == ENC_BYTES) {
        CandoString *s = cando_string_new((const char *)digest, (uint32_t)dn);
        cando_vm_push(vm, cando_string_value(s));
        return 1;
    }
    /* hex */
    char out[129];                          /* enough for SHA-512 */
    if (dn * 2 + 1 > sizeof(out)) {
        return canboot_error_throw(vm, "ENOMEM",
            "%s: digest too large for hex buffer", for_call);
    }
    hex_encode(digest, dn, out);
    CandoString *s = cando_string_new(out, (uint32_t)(dn * 2));
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* ---- Hash dispatcher ------------------------------------------------- */

static int do_hash(CandoVM *vm,
                   mbedtls_md_type_t algo, const char *for_call,
                   const char *data, size_t dn,
                   CryptoEnc enc)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(algo);
    if (!info) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: digest not available in this build", for_call);
    }
    unsigned char digest[64];
    int rc = mbedtls_md(info, (const unsigned char *)data, dn, digest);
    if (rc != 0) {
        return canboot_error_throw(vm, "EIO",
            "%s: mbedtls_md failed (rc=%d)", for_call, rc);
    }
    return push_digest(vm, digest, mbedtls_md_get_size(info), enc, for_call);
}

static int hash_simple(CandoVM *vm, int argc, CandoValue *args,
                       mbedtls_md_type_t algo, const char *for_call)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    const char *data = s ? s->data   : "";
    size_t      dn   = s ? s->length : 0;
    CryptoEnc enc = enc_from_arg(args, argc, 1);
    return do_hash(vm, algo, for_call, data, dn, enc);
}

static int c_md5   (CandoVM *vm, int argc, CandoValue *args) { return hash_simple(vm, argc, args, MBEDTLS_MD_MD5,    "crypto.md5");    }
static int c_sha1  (CandoVM *vm, int argc, CandoValue *args) { return hash_simple(vm, argc, args, MBEDTLS_MD_SHA1,   "crypto.sha1");   }
static int c_sha224(CandoVM *vm, int argc, CandoValue *args) { return hash_simple(vm, argc, args, MBEDTLS_MD_SHA224, "crypto.sha224"); }
static int c_sha256(CandoVM *vm, int argc, CandoValue *args) { return hash_simple(vm, argc, args, MBEDTLS_MD_SHA256, "crypto.sha256"); }
static int c_sha384(CandoVM *vm, int argc, CandoValue *args) { return hash_simple(vm, argc, args, MBEDTLS_MD_SHA384, "crypto.sha384"); }
static int c_sha512(CandoVM *vm, int argc, CandoValue *args) { return hash_simple(vm, argc, args, MBEDTLS_MD_SHA512, "crypto.sha512"); }

/* crypto.hash(algo, data, enc?) -> string */
static int c_hash(CandoVM *vm, int argc, CandoValue *args) {
    const char *algo_name = libutil_arg_cstr_at(args, argc, 0);
    if (!algo_name) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hash: first argument must be the algorithm name");
    }
    mbedtls_md_type_t algo = resolve_md(algo_name);
    if (algo == MBEDTLS_MD_NONE) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hash: unsupported algorithm '%s'", algo_name);
    }
    CandoString *s = libutil_arg_str_at(args, argc, 1);
    const char *data = s ? s->data   : "";
    size_t      dn   = s ? s->length : 0;
    CryptoEnc enc = enc_from_arg(args, argc, 2);
    return do_hash(vm, algo, "crypto.hash", data, dn, enc);
}

/* ---- HMAC dispatcher ------------------------------------------------- */

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

/* crypto.hmac(algo, key, data, enc?) -> string */
static int c_hmac(CandoVM *vm, int argc, CandoValue *args) {
    const char *algo_name = libutil_arg_cstr_at(args, argc, 0);
    if (!algo_name) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hmac: first argument must be the algorithm name");
    }
    mbedtls_md_type_t algo = resolve_md(algo_name);
    if (algo == MBEDTLS_MD_NONE) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hmac: unsupported algorithm '%s'", algo_name);
    }
    CandoString *ks = libutil_arg_str_at(args, argc, 1);
    CandoString *ds = libutil_arg_str_at(args, argc, 2);
    if (!ks || !ds) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hmac: key and data required");
    }
    CryptoEnc enc = enc_from_arg(args, argc, 3);

    unsigned char digest[64];
    int rc = hmac_compute(algo,
                          (const unsigned char *)ks->data, ks->length,
                          (const unsigned char *)ds->data, ds->length,
                          digest);
    if (rc != 0) {
        return canboot_error_throw(vm, "EIO",
            "crypto.hmac: mbedtls failed (rc=%d)", rc);
    }
    size_t dn = mbedtls_md_get_size(mbedtls_md_info_from_type(algo));
    return push_digest(vm, digest, dn, enc, "crypto.hmac");
}

/* ---- crypto.hex.encode / decode ------------------------------------- */

static int c_hex_encode(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hex.encode: bytes argument required");
    }
    /* Encode in-place to a stack buffer; cap protects against huge inputs. */
    if ((size_t)s->length * 2 > 8192) {
        return canboot_error_throw(vm, "ENOMEM",
            "crypto.hex.encode: input too large (max 4096 bytes)");
    }
    char out[8192];
    hex_encode((const unsigned char *)s->data, s->length, out);
    CandoString *r = cando_string_new(out, s->length * 2);
    cando_vm_push(vm, cando_string_value(r));
    return 1;
}

static int c_hex_decode(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hex.decode: hex string argument required");
    }
    if (s->length / 2 > 4096) {
        return canboot_error_throw(vm, "ENOMEM",
            "crypto.hex.decode: input too large (max 8192 chars)");
    }
    unsigned char out[4096];
    int n = hex_decode(s->data, s->length, out, sizeof(out));
    if (n < 0) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hex.decode: malformed hex input");
    }
    CandoString *r = cando_string_new((const char *)out, (uint32_t)n);
    cando_vm_push(vm, cando_string_value(r));
    return 1;
}

/* ---- crypto.timingSafeEqual ----------------------------------------- */

static int c_timing_safe_equal(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *a = libutil_arg_str_at(args, argc, 0);
    CandoString *b = libutil_arg_str_at(args, argc, 1);
    if (!a || !b) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    if (a->length != b->length) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    unsigned char diff = 0;
    for (uint32_t i = 0; i < a->length; i++) {
        diff |= (unsigned char)a->data[i] ^ (unsigned char)b->data[i];
    }
    cando_vm_push(vm, cando_bool(diff == 0));
    return 1;
}

/* ---- Legacy aliases (raw + Hex) ------------------------------------- */
/* Kept so existing smoke-test markers + scripts coded to the older
 * canboot crypto surface keep working. New code should use the
 * CanDo drop-in surface above (crypto.sha256, crypto.hmac, etc.). */

static int c_sha256_raw_legacy(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    const char *data = s ? s->data   : "";
    size_t      dn   = s ? s->length : 0;
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char *)data, dn, digest, 0) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *r = cando_string_new((const char *)digest, 32);
    cando_vm_push(vm, cando_string_value(r));
    return 1;
}

static int c_sha512_raw_legacy(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    const char *data = s ? s->data   : "";
    size_t      dn   = s ? s->length : 0;
    unsigned char digest[64];
    if (mbedtls_sha512((const unsigned char *)data, dn, digest, 0) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *r = cando_string_new((const char *)digest, 64);
    cando_vm_push(vm, cando_string_value(r));
    return 1;
}

static int c_sha256_hex_legacy(CandoVM *vm, int argc, CandoValue *args) {
    return hash_simple(vm, argc, args, MBEDTLS_MD_SHA256, "crypto.sha256Hex");
}

static int c_sha512_hex_legacy(CandoVM *vm, int argc, CandoValue *args) {
    return hash_simple(vm, argc, args, MBEDTLS_MD_SHA512, "crypto.sha512Hex");
}

static int c_hmac_sha256_legacy(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *ks = libutil_arg_str_at(args, argc, 0);
    CandoString *ds = libutil_arg_str_at(args, argc, 1);
    if (!ks || !ds) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char digest[32];
    if (hmac_compute(MBEDTLS_MD_SHA256,
                     (const unsigned char *)ks->data, ks->length,
                     (const unsigned char *)ds->data, ds->length, digest) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *r = cando_string_new((const char *)digest, 32);
    cando_vm_push(vm, cando_string_value(r));
    return 1;
}

static int c_hmac_sha256_hex_legacy(CandoVM *vm, int argc, CandoValue *args) {
    /* Old surface was crypto.hmacSha256Hex(key, data); the new dispatcher
     * is crypto.hmac("sha256", key, data, "hex"). Adapt by prepending
     * a synthetic algo arg. */
    CandoValue tmp[4];
    tmp[0] = cando_string_value(cando_string_new("sha256", 6));
    tmp[1] = (argc > 0) ? args[0] : cando_null();
    tmp[2] = (argc > 1) ? args[1] : cando_null();
    tmp[3] = cando_string_value(cando_string_new("hex", 3));
    return c_hmac(vm, 4, tmp);
}

/* ---- Registration --------------------------------------------------- */

static const LibutilMethodEntry crypto_methods[] = {
    /* CanDo drop-in surface. */
    { "md5",             c_md5                  },
    { "sha1",            c_sha1                 },
    { "sha224",          c_sha224               },
    { "sha256",          c_sha256               },
    { "sha384",          c_sha384               },
    { "sha512",          c_sha512               },
    { "hash",            c_hash                 },
    { "hmac",            c_hmac                 },
    { "timingSafeEqual", c_timing_safe_equal    },

    /* Legacy canboot-specific aliases (kept for smoke tests + existing
     * scripts coded to the older surface). */
    { "sha256Hex",       c_sha256_hex_legacy    },
    { "sha512Hex",       c_sha512_hex_legacy    },
    { "hmacSha256",      c_hmac_sha256_legacy   },
    { "hmacSha256Hex",   c_hmac_sha256_hex_legacy },
    { "sha256Raw",       c_sha256_raw_legacy    },  /* raw 32-byte output */
    { "sha512Raw",       c_sha512_raw_legacy    },  /* raw 64-byte output */
};

void canboot_cando_open_cryptolib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, crypto_methods,
                             sizeof(crypto_methods) / sizeof(crypto_methods[0]));

    /* crypto.hex sub-namespace. */
    CandoValue hex_val = cando_bridge_new_object(vm);
    CdoObject *hex_obj = cando_bridge_resolve(vm, cando_as_handle(hex_val));
    libutil_set_method(vm, hex_obj, "encode", c_hex_encode);
    libutil_set_method(vm, hex_obj, "decode", c_hex_decode);

    /* Attach crypto.hex as a field on the crypto object. Field key is
     * an interned CdoString; cdo_object_rawset retains the value's
     * inner refs so we release the key after the call. */
    CdoString *k = cdo_string_intern("hex", 3);
    cdo_object_rawset(obj, k,
                      cando_bridge_to_cdo(vm, hex_val),
                      FIELD_NONE);
    cdo_string_release(k);

    cando_vm_set_global(vm, "crypto", obj_val, true);
}
