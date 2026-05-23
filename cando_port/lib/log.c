/*
 * cando log module - structured serial logging with severity prefix
 * and millisecond timestamp. Routed through hal_console so output
 * shows up on the same UART our smoke tests read.
 *
 *   log.info(...)    -> "[<ms>] INFO  <args>"
 *   log.warn(...)    -> "[<ms>] WARN  <args>"
 *   log.error(...)   -> "[<ms>] ERROR <args>"
 *   log.debug(...)   -> "[<ms>] DEBUG <args>"
 *   log.setLevel(s)  -> "debug" | "info" | "warn" | "error" / "off"
 *
 * Each call takes a single string argument; richer formatting belongs
 * to whatever the caller assembles before invoking us. Multi-arg
 * variants would need cando vararg unpacking which is out of scope.
 *
 * Severity ordering: debug < info < warn < error. setLevel(level)
 * suppresses any call below that level. Default level is "info".
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "lwip/sys.h"
#include "hal/console.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

enum { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_OFF };
static int g_min_level = LOG_INFO;

static const char *level_name(int lvl) {
    switch (lvl) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        default:        return "?    ";
    }
}

static int parse_level(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcmp(s, "info")  == 0) return LOG_INFO;
    if (strcmp(s, "warn")  == 0) return LOG_WARN;
    if (strcmp(s, "error") == 0) return LOG_ERROR;
    if (strcmp(s, "off")   == 0) return LOG_OFF;
    return -1;
}

static void emit(int lvl, const char *msg) {
    if (lvl < g_min_level) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "[%10u] %s ",
                     (unsigned)sys_now(), level_name(lvl));
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(buf)) n = (int)sizeof(buf);
    hal_console_write_n(buf, (size_t)n);
    if (msg) hal_console_write(msg);
    hal_console_putc('\n');
}

static int l_debug(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    emit(LOG_DEBUG, libutil_arg_cstr_at(args, argc, 0));
    return 0;
}
static int l_info(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    emit(LOG_INFO,  libutil_arg_cstr_at(args, argc, 0));
    return 0;
}
static int l_warn(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    emit(LOG_WARN,  libutil_arg_cstr_at(args, argc, 0));
    return 0;
}
static int l_error(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    emit(LOG_ERROR, libutil_arg_cstr_at(args, argc, 0));
    return 0;
}

static int l_set_level(CandoVM *vm, int argc, CandoValue *args) {
    const char *s = libutil_arg_cstr_at(args, argc, 0);
    int lvl = parse_level(s);
    if (lvl < 0) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    g_min_level = lvl;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static const LibutilMethodEntry log_methods[] = {
    { "debug",    l_debug    },
    { "info",     l_info     },
    { "warn",     l_warn     },
    { "error",    l_error    },
    { "setLevel", l_set_level },
};

void canboot_cando_open_loglib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, log_methods,
                             sizeof(log_methods) / sizeof(log_methods[0]));
    cando_vm_set_global(vm, "log", obj_val, true);
}
