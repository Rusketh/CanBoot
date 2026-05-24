/*
 * cando_port/lib/error.c — Error CLASS for canboot bindings.
 *
 * Drop-in / extension note:
 *   Host CanDo signals errors via cando_vm_error(); there's no Error
 *   object type in upstream cando stdlib. The canboot binding layer
 *   wants a structured error value so the canboot-specific (value, err)
 *   tuple convention (used by libs that have no host CanDo counterpart
 *   — audio, image, disk, etc.) can carry { code, message, cause }
 *   instead of bare strings or booleans.
 *
 *   For libs that DO have a host CanDo counterpart (file, net, http,
 *   ...) the error model is throw-based via cando_vm_error to remain
 *   drop-in for host CanDo scripts. The Error CLASS here is used only
 *   in places where the (value, err) tuple is the right shape.
 *
 *   Either convention can reach the same Error object: Group A libs
 *   call canboot_error_push_throw() which formats the message into
 *   cando_vm_error and returns -1. Group B libs call
 *   canboot_error_push() which pushes the Error value as the err half
 *   of the tuple.
 *
 * Instance layout (CdoObject fields):
 *   .__error_code     string   POSIX-shaped code (ENOENT, EIO, ...)
 *   .__error_message  string   human-readable message
 *   .__error_cause    any|nil  optional nested cause
 *
 * Meta table _meta.error (method dispatch via __index):
 *   :code()      -> string
 *   :message()   -> string
 *   :cause()     -> Error|nil
 *   :toString()  -> "CODE: message"
 *
 * Module surface:
 *   Error.new(code, message[, cause]) -> Error
 *
 * Conventional codes (not enforced; libs use whatever fits):
 *   ENOENT, EIO, EAGAIN, ETIMEDOUT, EINVAL, ENOMEM, EPERM,
 *   ENETUNREACH, EHOSTUNREACH, ECONNREFUSED, ETLSHANDSHAKE,
 *   EBADMSG, EBUSY, ENOSPC, EEXIST, EISDIR, ENOTDIR
 */

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/meta.h"
#include "lib/object.h"

/* ---------------------------------------------------------------------- */
/* Field key cache. We intern the field names once and hold them for the  */
/* process lifetime to avoid re-interning on every error construction.    */
/* ---------------------------------------------------------------------- */

static CdoString *k_code    = NULL;
static CdoString *k_message = NULL;
static CdoString *k_cause   = NULL;

static void ensure_keys(void) {
    if (!k_code)    k_code    = cdo_string_intern("__error_code",    12);
    if (!k_message) k_message = cdo_string_intern("__error_message", 15);
    if (!k_cause)   k_cause   = cdo_string_intern("__error_cause",   13);
}

/* ---------------------------------------------------------------------- */
/* Construction                                                           */
/* ---------------------------------------------------------------------- */

static CandoValue make_error(CandoVM *vm,
                             const char *code,
                             const char *message,
                             CandoValue cause)
{
    ensure_keys();

    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(val));

    /* code */
    if (code) {
        CdoString *s = cdo_string_new(code, (uint32_t)strlen(code));
        CdoValue   v = cdo_string_value(s);
        cdo_object_rawset(obj, k_code, v, FIELD_NONE);
        cdo_value_release(v);
    } else {
        cdo_object_rawset(obj, k_code, cdo_null(), FIELD_NONE);
    }

    /* message */
    if (message) {
        CdoString *s = cdo_string_new(message, (uint32_t)strlen(message));
        CdoValue   v = cdo_string_value(s);
        cdo_object_rawset(obj, k_message, v, FIELD_NONE);
        cdo_value_release(v);
    } else {
        cdo_object_rawset(obj, k_message, cdo_null(), FIELD_NONE);
    }

    /* cause */
    cdo_object_rawset(obj, k_cause,
                      cando_bridge_to_cdo(vm, cause),
                      FIELD_NONE);

    cando_lib_meta_attach(vm, obj, "error");
    return val;
}

/* Public C helpers. Callers from other cando_port/lib bindings use these. */

CandoValue canboot_error_value(CandoVM *vm,
                               const char *code,
                               const char *message)
{
    return make_error(vm, code, message, cando_null());
}

int canboot_error_push(CandoVM *vm,
                       const char *code,
                       const char *message)
{
    cando_vm_push(vm, make_error(vm, code, message, cando_null()));
    return 1;
}

/* Throw-flavour for Group A libs: format the message into cando_vm_error
 * and return -1 from CandoNativeFn. The code is prefixed onto the
 * message so it survives the throw->script path even though host CanDo
 * has no Error object type. */
int canboot_error_throw(CandoVM *vm, const char *code, const char *fmt, ...)
{
    char buf[256];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    } else {
        buf[0] = '\0';
    }
    cando_vm_error(vm, "%s: %s",
                   code ? code : "EINVAL",
                   buf[0] ? buf : "(unspecified)");
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Cando bindings                                                         */
/* ---------------------------------------------------------------------- */

/* Look up a field on the object's own slots; returns cdo_null if absent. */
static CdoValue field_get(CdoObject *obj, CdoString *key) {
    CdoValue out;
    if (cdo_object_rawget(obj, key, &out)) return out;
    return cdo_null();
}

/* Resolve `self` from the bound method receiver. CanDo passes the
 * receiver as args[0] for method calls. */
static CdoObject *self_obj(CandoVM *vm, int argc, CandoValue *args,
                           const char *method)
{
    if (argc < 1) {
        cando_vm_error(vm, "Error:%s: missing receiver", method);
        return NULL;
    }
    if (!cando_is_object(args[0])) {
        cando_vm_error(vm, "Error:%s: receiver is not an object", method);
        return NULL;
    }
    return cando_bridge_resolve(vm, cando_as_handle(args[0]));
}

static int m_code(CandoVM *vm, int argc, CandoValue *args) {
    ensure_keys();
    CdoObject *obj = self_obj(vm, argc, args, "code");
    if (!obj) return -1;
    cando_vm_push(vm, cando_bridge_to_cando(vm, field_get(obj, k_code)));
    return 1;
}

static int m_message(CandoVM *vm, int argc, CandoValue *args) {
    ensure_keys();
    CdoObject *obj = self_obj(vm, argc, args, "message");
    if (!obj) return -1;
    cando_vm_push(vm, cando_bridge_to_cando(vm, field_get(obj, k_message)));
    return 1;
}

static int m_cause(CandoVM *vm, int argc, CandoValue *args) {
    ensure_keys();
    CdoObject *obj = self_obj(vm, argc, args, "cause");
    if (!obj) return -1;
    cando_vm_push(vm, cando_bridge_to_cando(vm, field_get(obj, k_cause)));
    return 1;
}

static int m_to_string(CandoVM *vm, int argc, CandoValue *args) {
    ensure_keys();
    CdoObject *obj = self_obj(vm, argc, args, "toString");
    if (!obj) return -1;

    CdoValue cv = field_get(obj, k_code);
    CdoValue mv = field_get(obj, k_message);

    /* CdoValue is a tagged union; for string-typed values the payload
     * lives in .as.string (a CdoString * with .data NUL-terminated). */
    const char *code_s =
        (cdo_is_string(cv) && cv.as.string) ? cv.as.string->data : NULL;
    const char *msg_s  =
        (cdo_is_string(mv) && mv.as.string) ? mv.as.string->data : NULL;

    char buf[320];
    int n = snprintf(buf, sizeof(buf), "%s: %s",
                     code_s ? code_s : "ERROR",
                     msg_s  ? msg_s  : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;

    libutil_push_cstr(vm, buf);
    return 1;
}

/* Module-level constructor: Error.new(code, message[, cause]) -> Error */
static int mod_new(CandoVM *vm, int argc, CandoValue *args) {
    /* Cando passes the module as args[0] for `Error.new(...)` calls when
     * registered as a method. Skip it. */
    int base = (argc > 0 && cando_is_object(args[0])) ? 1 : 0;

    const char *code = libutil_arg_cstr_at(args, argc, base + 0);
    const char *msg  = libutil_arg_cstr_at(args, argc, base + 1);
    CandoValue cause = (argc > base + 2) ? args[base + 2] : cando_null();

    if (!code || !*code) {
        cando_vm_error(vm, "Error.new: code (string) required");
        return -1;
    }

    cando_vm_push(vm, make_error(vm, code, msg, cause));
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Registration                                                           */
/* ---------------------------------------------------------------------- */

void canboot_cando_open_errorlib(CandoVM *vm) {
    /* _meta global must exist before we hang methods off _meta.error.
     * cando_openlibs() already calls cando_lib_meta_register so this is
     * a no-op in practice; kept for forward-compat if the canboot port
     * is ever invoked outside the standard openlibs flow. */
    cando_lib_meta_register(vm);

    /* _meta.error — method dispatch table for Error instances. */
    CdoObject *meta = cando_lib_meta_table(vm, "error");
    if (meta) {
        cando_lib_meta_define(vm, meta, "code",     m_code);
        cando_lib_meta_define(vm, meta, "message",  m_message);
        cando_lib_meta_define(vm, meta, "cause",    m_cause);
        cando_lib_meta_define(vm, meta, "toString", m_to_string);
    }

    /* Global `Error` module with `.new(code, message[, cause])`. */
    CandoValue mod = cando_bridge_new_object(vm);
    CdoObject *mod_obj = cando_bridge_resolve(vm, cando_as_handle(mod));
    libutil_set_method(vm, mod_obj, "new", mod_new);
    cando_vm_set_global(vm, "Error", mod, true);
}
