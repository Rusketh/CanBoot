/*
 * cando_port/lib/error.h — Error CLASS public C surface.
 *
 * Used by other canboot binding files to produce structured errors
 * with consistent shape. See error.c for the script-facing Error
 * class layout (`__error_code` / `__error_message` / `__error_cause`
 * fields + `_meta.error` method table).
 *
 * Two conventions live alongside:
 *
 *   - Group A libs (file, net, http, https, crypto, ...) match host
 *     CanDo's throw-based error model. Use canboot_error_throw():
 *
 *         if (!path)
 *             return canboot_error_throw(vm, "EINVAL",
 *                 "file.read: path required");
 *
 *   - Group B libs (audio, image, disk, ...) return a (value, err)
 *     tuple. On the err half, use canboot_error_push() inside the
 *     CandoNativeFn (it pushes one value; caller pushes the value
 *     half first, then calls this, then returns 2):
 *
 *         cando_vm_push(vm, cando_null());          // value half
 *         return 1 + canboot_error_push(vm, "EIO",
 *             "audio.play: source pool exhausted");
 *
 *   - canboot_error_value() returns a CandoValue suitable for hand-
 *     placing into compound returns (e.g. inside an array or as a
 *     stored field).
 *
 * Callers must include "core/value.h" (for CandoValue) and have a
 * declaration of CandoVM in scope. error.h does not pull those in
 * to stay light; consumers already include them for their normal
 * binding work.
 */

#ifndef CANBOOT_LIB_ERROR_H
#define CANBOOT_LIB_ERROR_H

#include "core/value.h"

struct CandoVM;

/* Construct an Error instance without pushing it. */
CandoValue canboot_error_value(struct CandoVM *vm,
                               const char *code,
                               const char *message);

/* Push an Error instance onto the VM stack. Returns 1 (one value
 * pushed) so callers can write `return canboot_error_push(...)` or
 * `return n + canboot_error_push(...)` for tuple returns. */
int canboot_error_push(struct CandoVM *vm,
                       const char *code,
                       const char *message);

/* Throw via cando_vm_error formatted as "<code>: <message>". Returns
 * -1 so CandoNativeFn impls can `return canboot_error_throw(...)`. */
int canboot_error_throw(struct CandoVM *vm,
                        const char *code,
                        const char *fmt, ...);

/* Register the Error CLASS + module global. Called once at boot
 * from the selftest cando bring-up. */
void canboot_cando_open_errorlib(struct CandoVM *vm);

#endif /* CANBOOT_LIB_ERROR_H */
