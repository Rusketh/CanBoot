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
#include <stdbool.h>
#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/cipher.h"

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

/* ---- crypto.randomBytes / randomInt / randomUUID -------------------- */
/*
 * Mbed TLS CTR-DRBG seeded once from mbedtls_entropy_func. Same RNG
 * source the canboot `random` module uses; this lib also exposes it
 * under CanDo's `crypto.*` names so a script written for host CanDo
 * doesn't need to reach for two different namespaces.
 */

static mbedtls_entropy_context  g_rng_ent;
static mbedtls_ctr_drbg_context g_rng_drbg;
static int                      g_rng_seeded;

static int rng_seed(void) {
    if (g_rng_seeded) return 0;
    mbedtls_entropy_init(&g_rng_ent);
    mbedtls_ctr_drbg_init(&g_rng_drbg);
    int rc = mbedtls_ctr_drbg_seed(&g_rng_drbg, mbedtls_entropy_func, &g_rng_ent,
                                   (const unsigned char *)"canboot-crypto", 14);
    if (rc != 0) return rc;
    g_rng_seeded = 1;
    return 0;
}

static int rng_draw(unsigned char *out, size_t n) {
    if (rng_seed() != 0) return -1;
    while (n) {
        size_t chunk = n > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : n;
        if (mbedtls_ctr_drbg_random(&g_rng_drbg, out, chunk) != 0) return -1;
        out += chunk;
        n   -= chunk;
    }
    return 0;
}

/* crypto.randomBytes(n, enc?) -> string (default enc "hex") */
static int c_random_bytes(CandoVM *vm, int argc, CandoValue *args) {
    int n = (int)libutil_arg_num_at(args, argc, 0, 16);
    if (n < 0 || n > 4096) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.randomBytes: size must be 0..4096");
    }
    static unsigned char buf[4096];
    if (rng_draw(buf, (size_t)n) != 0) {
        return canboot_error_throw(vm, "EIO",
            "crypto.randomBytes: DRBG generation failed");
    }
    CryptoEnc enc = enc_from_arg(args, argc, 1);
    return push_digest(vm, buf, (size_t)n, enc, "crypto.randomBytes");
}

/* crypto.randomInt(min, max) -> number — uniform in [min, max] */
static int c_random_int(CandoVM *vm, int argc, CandoValue *args) {
    int64_t lo = (int64_t)libutil_arg_num_at(args, argc, 0, 0);
    int64_t hi = (int64_t)libutil_arg_num_at(args, argc, 1, lo + 1);
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)(hi - lo) + 1ull;
    uint64_t r;
    if (rng_draw((unsigned char *)&r, sizeof(r)) != 0) {
        return canboot_error_throw(vm, "EIO",
            "crypto.randomInt: DRBG generation failed");
    }
    int64_t pick = lo + (int64_t)(r % span);
    cando_vm_push(vm, cando_number((f64)pick));
    return 1;
}

/* crypto.randomUUID() -> string (RFC 4122 v4, 36 chars with dashes) */
static int c_random_uuid(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    unsigned char b[16];
    if (rng_draw(b, 16) != 0) {
        return canboot_error_throw(vm, "EIO",
            "crypto.randomUUID: DRBG generation failed");
    }
    /* v4 bit-twiddling per RFC 4122. */
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

/* ---- crypto.base64 / crypto.base64url ------------------------------- */
/*
 * Standard alphabet plus the RFC 4648 URL-safe variant ('+' -> '-',
 * '/' -> '_'). Both encoders pad with '='; the url-safe decoder also
 * accepts unpadded input (common in JWT contexts).
 */

static const char b64_std[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64_url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64_value_std(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_value_url(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int do_b64_encode(CandoVM *vm, int argc, CandoValue *args,
                         const char *alphabet, const char *for_call)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: bytes argument required", for_call);
    }
    if ((size_t)s->length > 6144) {
        return canboot_error_throw(vm, "ENOMEM",
            "%s: input too large (max 6144 bytes)", for_call);
    }
    static char out[8192];
    const unsigned char *src = (const unsigned char *)s->data;
    size_t n = s->length;
    size_t out_n = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)src[i]   << 16)
                   | ((uint32_t)src[i+1] <<  8)
                   |  (uint32_t)src[i+2];
        out[out_n++] = alphabet[(v >> 18) & 0x3F];
        out[out_n++] = alphabet[(v >> 12) & 0x3F];
        out[out_n++] = alphabet[(v >>  6) & 0x3F];
        out[out_n++] = alphabet[(v      ) & 0x3F];
        i += 3;
    }
    if (i + 1 == n) {
        uint32_t v = (uint32_t)src[i] << 16;
        out[out_n++] = alphabet[(v >> 18) & 0x3F];
        out[out_n++] = alphabet[(v >> 12) & 0x3F];
        out[out_n++] = '=';
        out[out_n++] = '=';
    } else if (i + 2 == n) {
        uint32_t v = ((uint32_t)src[i]   << 16)
                   | ((uint32_t)src[i+1] <<  8);
        out[out_n++] = alphabet[(v >> 18) & 0x3F];
        out[out_n++] = alphabet[(v >> 12) & 0x3F];
        out[out_n++] = alphabet[(v >>  6) & 0x3F];
        out[out_n++] = '=';
    }
    CandoString *r = cando_string_new(out, (uint32_t)out_n);
    cando_vm_push(vm, cando_string_value(r));
    return 1;
}

static int do_b64_decode(CandoVM *vm, int argc, CandoValue *args,
                         int (*val_fn)(char), bool tolerate_unpadded,
                         const char *for_call)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: string argument required", for_call);
    }
    const char *src = s->data;
    size_t n = s->length;

    /* For URL-safe input we synthesise the padding so the rest of the
     * loop is mod-4. Standard base64 requires correct padding. */
    char padded[8192];
    if (tolerate_unpadded && (n % 4) != 0) {
        size_t pad = 4 - (n % 4);
        if (n + pad > sizeof(padded)) {
            return canboot_error_throw(vm, "ENOMEM",
                "%s: input too large (max %zu chars)", for_call,
                sizeof(padded) - 4);
        }
        memcpy(padded, src, n);
        for (size_t k = 0; k < pad; k++) padded[n + k] = '=';
        src = padded;
        n  += pad;
    }
    if (n % 4 != 0) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: length not a multiple of 4", for_call);
    }
    static char out[6144];
    if ((n / 4) * 3 > sizeof(out)) {
        return canboot_error_throw(vm, "ENOMEM",
            "%s: input too large for output buffer", for_call);
    }
    size_t out_n = 0;
    for (size_t i = 0; i < n; i += 4) {
        int a = val_fn(src[i]);
        int b = val_fn(src[i + 1]);
        int c = src[i + 2] == '=' ? -2 : val_fn(src[i + 2]);
        int d = src[i + 3] == '=' ? -2 : val_fn(src[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            return canboot_error_throw(vm, "EINVAL",
                "%s: malformed input", for_call);
        }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12)
                   | ((uint32_t)(c < 0 ? 0 : c) << 6) | (uint32_t)(d < 0 ? 0 : d);
        out[out_n++] = (char)((v >> 16) & 0xFF);
        if (c >= 0) out[out_n++] = (char)((v >> 8) & 0xFF);
        if (d >= 0) out[out_n++] = (char)( v       & 0xFF);
    }
    CandoString *r = cando_string_new(out, (uint32_t)out_n);
    cando_vm_push(vm, cando_string_value(r));
    return 1;
}

static int c_b64_encode    (CandoVM *vm, int argc, CandoValue *args)
{ return do_b64_encode(vm, argc, args, b64_std, "crypto.base64.encode"); }

static int c_b64_decode    (CandoVM *vm, int argc, CandoValue *args)
{ return do_b64_decode(vm, argc, args, b64_value_std, false, "crypto.base64.decode"); }

static int c_b64url_encode (CandoVM *vm, int argc, CandoValue *args)
{ return do_b64_encode(vm, argc, args, b64_url, "crypto.base64url.encode"); }

static int c_b64url_decode (CandoVM *vm, int argc, CandoValue *args)
{ return do_b64_decode(vm, argc, args, b64_value_url, true, "crypto.base64url.decode"); }

/* ---- crypto.pbkdf2 / crypto.hkdf ------------------------------------ */
/*
 * Standard CanDo signatures (vendor/cando/source/lib/crypto.c lines
 * 1956-1958):
 *
 *   crypto.pbkdf2(password, salt, iterations, keylen,
 *                 digest = "sha256", enc = "hex") -> string
 *   crypto.hkdf(secret, salt, info, length,
 *               digest = "sha256", enc = "hex") -> string
 *
 * Defaults follow CanDo's docs: SHA-256 digest, hex output. Output
 * length cap (256 bytes) is per call — large enough for an X25519
 * keypair seed or a TLS 1.3 traffic key while keeping a static
 * scratch buffer on the freestanding heap.
 */

/* Resolve the digest arg or default to SHA-256. */
static mbedtls_md_type_t kdf_digest(CandoValue *args, int argc, int idx,
                                    const char *for_call, CandoVM *vm,
                                    int *err)
{
    *err = 0;
    const char *name = libutil_arg_cstr_at(args, argc, idx);
    if (!name || !*name) return MBEDTLS_MD_SHA256;
    mbedtls_md_type_t md = resolve_md(name);
    if (md == MBEDTLS_MD_NONE) {
        *err = canboot_error_throw(vm, "EINVAL",
            "%s: unsupported digest '%s'", for_call, name);
    }
    return md;
}

static int c_pbkdf2(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *ps = libutil_arg_str_at(args, argc, 0);
    CandoString *ss = libutil_arg_str_at(args, argc, 1);
    if (!ps || !ss) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.pbkdf2: password and salt required");
    }
    int iter = (int)libutil_arg_num_at(args, argc, 2, 1);
    int klen = (int)libutil_arg_num_at(args, argc, 3, 32);
    if (iter < 1 || iter > 10000000) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.pbkdf2: iterations must be 1..10000000");
    }
    if (klen < 1 || klen > 256) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.pbkdf2: keylen must be 1..256");
    }

    int derr = 0;
    mbedtls_md_type_t md = kdf_digest(args, argc, 4, "crypto.pbkdf2", vm, &derr);
    if (derr) return derr;
    CryptoEnc enc = enc_from_arg(args, argc, 5);

    unsigned char out[256];
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        md,
        (const unsigned char *)ps->data, ps->length,
        (const unsigned char *)ss->data, ss->length,
        (unsigned)iter, (uint32_t)klen, out);
    if (rc != 0) {
        return canboot_error_throw(vm, "EIO",
            "crypto.pbkdf2: mbedtls failed (rc=%d)", rc);
    }
    return push_digest(vm, out, (size_t)klen, enc, "crypto.pbkdf2");
}

static int c_hkdf(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *secret = libutil_arg_str_at(args, argc, 0);
    CandoString *salt   = libutil_arg_str_at(args, argc, 1);
    CandoString *info   = libutil_arg_str_at(args, argc, 2);
    if (!secret) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hkdf: secret (input keying material) required");
    }
    int len = (int)libutil_arg_num_at(args, argc, 3, 32);
    if (len < 1 || len > 256) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hkdf: length must be 1..256");
    }
    int derr = 0;
    mbedtls_md_type_t md = kdf_digest(args, argc, 4, "crypto.hkdf", vm, &derr);
    if (derr) return derr;
    CryptoEnc enc = enc_from_arg(args, argc, 5);

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md);
    if (!md_info) {
        return canboot_error_throw(vm, "EINVAL",
            "crypto.hkdf: digest not available in this build");
    }

    unsigned char out[256];
    int rc = mbedtls_hkdf(
        md_info,
        salt   ? (const unsigned char *)salt->data   : NULL,
        salt   ? salt->length                        : 0,
        (const unsigned char *)secret->data, secret->length,
        info   ? (const unsigned char *)info->data   : NULL,
        info   ? info->length                        : 0,
        out, (size_t)len);
    if (rc != 0) {
        return canboot_error_throw(vm, "EIO",
            "crypto.hkdf: mbedtls failed (rc=%d)", rc);
    }
    return push_digest(vm, out, (size_t)len, enc, "crypto.hkdf");
}

/* ---- crypto.encrypt / crypto.decrypt -------------------------------- */
/*
 * CanDo signature (vendor/cando/source/lib/crypto.c lines 755-):
 *
 *   crypto.encrypt(algo, key, iv, data, aad?, encoding?) -> string|object
 *   crypto.decrypt(algo, key, iv, data, aad?, encoding?) -> string
 *
 * Non-AEAD modes (CBC, CTR) return the encoded ciphertext directly.
 * AEAD modes (GCM, ChaCha20-Poly1305) return { ciphertext, tag } —
 * tag is a 16-byte auth tag that crypto.decrypt also expects.
 *
 * THIS COMMIT scopes to non-AEAD ciphers only (AES-128/192/256-CBC,
 * AES-128/192/256-CTR). AEAD lands in a follow-up so the
 * object-return shape gets focused review.
 *
 *   algo:     "aes-128-cbc" | "aes-192-cbc" | "aes-256-cbc" |
 *             "aes-128-ctr" | "aes-192-ctr" | "aes-256-ctr"
 *   key:      bytes (16 / 24 / 32 for AES-128 / 192 / 256)
 *   iv:       16 raw bytes
 *   data:     plaintext (encrypt) or ciphertext (decrypt)
 *   aad:      reserved (ignored for non-AEAD; throws if non-empty
 *             with a non-AEAD cipher to surface mistakes)
 *   encoding: "hex" | "bytes" — default "bytes" per CanDo
 *
 * Block-cipher padding is PKCS#7 (CBC default in mbedtls). CTR is
 * unpadded (stream-cipher mode). The total output buffer is sized
 * to plaintext + block + safety; capped at 8192 bytes for the
 * static scratch buffer.
 */

static const mbedtls_cipher_info_t *cipher_info_from(const char *algo) {
    if (!algo) return NULL;
    /* Mbed TLS expects upper-case names ("AES-256-CBC"); convert. */
    char up[32];
    size_t i = 0;
    for (; algo[i] && i + 1 < sizeof(up); i++) {
        char c = algo[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        up[i] = c;
    }
    up[i] = '\0';
    return mbedtls_cipher_info_from_string(up);
}

static bool cipher_is_aead(const mbedtls_cipher_info_t *info) {
    mbedtls_cipher_mode_t mode = mbedtls_cipher_info_get_mode(info);
    return mode == MBEDTLS_MODE_GCM
        || mode == MBEDTLS_MODE_CCM
        || mode == MBEDTLS_MODE_CHACHAPOLY;
}

static CryptoEnc enc_from_arg_or_bytes(CandoValue *args, int argc, int idx) {
    /* Like enc_from_arg but bytes-by-default to match CanDo's encrypt /
     * decrypt convention (the data is binary). */
    const char *s = libutil_arg_cstr_at(args, argc, idx);
    if (!s) return ENC_BYTES;
    if (strcmp(s, "hex")    == 0) return ENC_HEX;
    if (strcmp(s, "bytes")  == 0) return ENC_BYTES;
    if (strcmp(s, "binary") == 0) return ENC_BYTES;
    return ENC_BAD;
}

static int run_cipher(CandoVM *vm, int op /* MBEDTLS_ENCRYPT / DECRYPT */,
                      int argc, CandoValue *args, const char *for_call)
{
    const char *algo_name = libutil_arg_cstr_at(args, argc, 0);
    CandoString *key  = libutil_arg_str_at(args, argc, 1);
    CandoString *iv   = libutil_arg_str_at(args, argc, 2);
    CandoString *data = libutil_arg_str_at(args, argc, 3);
    if (!algo_name || !key || !iv || !data) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: expected (algo, key, iv, data, aad?, encoding?)", for_call);
    }

    const mbedtls_cipher_info_t *info = cipher_info_from(algo_name);
    if (!info) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: unsupported cipher '%s'", for_call, algo_name);
    }
    if (cipher_is_aead(info)) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: AEAD modes (GCM/Poly1305) not yet supported "
            "— land in a follow-up commit", for_call);
    }

    size_t key_bytes = mbedtls_cipher_info_get_key_bitlen(info) / 8;
    size_t iv_bytes  = mbedtls_cipher_info_get_iv_size(info);
    if (key->length != key_bytes) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: key must be %zu bytes for %s (got %u)",
            for_call, key_bytes, algo_name, key->length);
    }
    if (iv->length != iv_bytes) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: iv must be %zu bytes for %s (got %u)",
            for_call, iv_bytes, algo_name, iv->length);
    }

    /* aad (arg 4) is reserved for AEAD; non-AEAD ignores. encoding is
     * arg 5 (or 4 when aad is absent). Detect which by length of the
     * positional. */
    int enc_idx = 4;
    CandoString *aad = libutil_arg_str_at(args, argc, 4);
    if (aad && aad->length > 0) {
        /* aad supplied with a non-AEAD cipher — surface the mistake
         * rather than silently dropping it. */
        return canboot_error_throw(vm, "EINVAL",
            "%s: aad provided with non-AEAD cipher '%s' — aad is "
            "only meaningful with GCM/Poly1305", for_call, algo_name);
    }
    if (aad) enc_idx = 5;
    CryptoEnc enc = enc_from_arg_or_bytes(args, argc, enc_idx);
    if (enc == ENC_BAD) {
        return canboot_error_throw(vm, "EINVAL",
            "%s: unsupported encoding (use 'hex' or 'bytes')", for_call);
    }

    if ((size_t)data->length > 8192 - 32) {
        return canboot_error_throw(vm, "ENOMEM",
            "%s: data too large (max 8160 bytes)", for_call);
    }

    mbedtls_cipher_context_t ctx;
    mbedtls_cipher_init(&ctx);
    int rc = mbedtls_cipher_setup(&ctx, info);
    if (rc == 0) {
        rc = mbedtls_cipher_setkey(&ctx,
            (const unsigned char *)key->data, (int)(key_bytes * 8),
            op == MBEDTLS_ENCRYPT ? MBEDTLS_ENCRYPT : MBEDTLS_DECRYPT);
    }
    if (rc == 0 && mbedtls_cipher_info_get_mode(info) == MBEDTLS_MODE_CBC) {
        rc = mbedtls_cipher_set_padding_mode(&ctx, MBEDTLS_PADDING_PKCS7);
    }
    if (rc != 0) {
        mbedtls_cipher_free(&ctx);
        return canboot_error_throw(vm, "EIO",
            "%s: cipher setup failed (rc=%d)", for_call, rc);
    }

    static unsigned char out_buf[8192];
    size_t out_len = sizeof(out_buf);
    rc = mbedtls_cipher_crypt(&ctx,
        (const unsigned char *)iv->data, iv_bytes,
        (const unsigned char *)data->data, data->length,
        out_buf, &out_len);
    mbedtls_cipher_free(&ctx);

    if (rc != 0) {
        return canboot_error_throw(vm, "EIO",
            "%s: cipher_crypt failed (rc=%d)", for_call, rc);
    }
    return push_digest(vm, out_buf, out_len, enc, for_call);
}

static int c_encrypt(CandoVM *vm, int argc, CandoValue *args) {
    return run_cipher(vm, MBEDTLS_ENCRYPT, argc, args, "crypto.encrypt");
}

static int c_decrypt(CandoVM *vm, int argc, CandoValue *args) {
    return run_cipher(vm, MBEDTLS_DECRYPT, argc, args, "crypto.decrypt");
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

    /* RNG — same DRBG source as the canboot `random` lib, also
     * exposed under CanDo's `crypto.*` names. */
    { "randomBytes",     c_random_bytes         },
    { "randomInt",       c_random_int           },
    { "randomUUID",      c_random_uuid          },

    /* KDFs — PBKDF2-HMAC and HKDF (RFC 5869). Output length capped
     * at 256 bytes per call. Defaults: SHA-256 digest, hex encoding. */
    { "pbkdf2",          c_pbkdf2               },
    { "hkdf",            c_hkdf                 },

    /* Symmetric ciphers — non-AEAD modes only (AES-CBC, AES-CTR).
     * AEAD (GCM, Poly1305) lands in a follow-up commit because of
     * its richer object return shape ({ciphertext, tag}). */
    { "encrypt",         c_encrypt              },
    { "decrypt",         c_decrypt              },

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

    /* Sub-namespaces: crypto.hex, crypto.base64, crypto.base64url.
     * Each lives as a field on the crypto object so calls dispatch
     * through the standard __index chain. */
    struct { const char *name; CandoNativeFn enc_fn; CandoNativeFn dec_fn; } subs[] = {
        { "hex",       c_hex_encode,    c_hex_decode    },
        { "base64",    c_b64_encode,    c_b64_decode    },
        { "base64url", c_b64url_encode, c_b64url_decode },
    };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        CandoValue sub_val = cando_bridge_new_object(vm);
        CdoObject *sub_obj = cando_bridge_resolve(vm, cando_as_handle(sub_val));
        libutil_set_method(vm, sub_obj, "encode", subs[i].enc_fn);
        libutil_set_method(vm, sub_obj, "decode", subs[i].dec_fn);
        CdoString *k = cdo_string_intern(subs[i].name,
                                          (uint32_t)strlen(subs[i].name));
        cdo_object_rawset(obj, k,
                          cando_bridge_to_cdo(vm, sub_val),
                          FIELD_NONE);
        cdo_string_release(k);
    }

    cando_vm_set_global(vm, "crypto", obj_val, true);
}
