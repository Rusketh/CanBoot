/*
 * cando_port/lib/os.c — drop-in for CanDo's `os` module on bare metal.
 *
 * Surface (matches vendor/cando/source/lib/os.c):
 *   os.getenv(name)          -> string | null
 *   os.setenv(name, val, ow?) -> bool
 *   os.execute(cmd)          -> throws (no subprocess on bare metal)
 *   os.exit(code?)           -> never returns; halts the kernel
 *   os.time()                -> number  (epoch seconds; RTC stub until HAL lands)
 *   os.clock()               -> number  (CPU seconds since boot)
 *   os.hostname()            -> string
 *   os.tmpdir()              -> string  ("/tmp" — see fs mount conventions)
 *   os.homedir()             -> string  ("/")
 *   os.arch()                -> "x86_64" | "aarch64"
 *   os.platform()            -> "canboot"
 *   os.uptime()              -> number  (seconds since boot)
 *   os.totalmem()            -> number  (bytes of CANBOOT_MMAP_USABLE)
 *   os.freemem()             -> number  (best-effort; same as totalmem until
 *                                        the kernel heap exposes a query)
 *   os.cpus()                -> array of one { model, speed } object
 *
 * Bare-metal limitations vs host CanDo:
 *   - No environ table: getenv returns null, setenv returns false.
 *     (A boot-cmdline scrape can be wired here later once kmain
 *     normalises CANBOOT cmdline key=value pairs.)
 *   - No subprocesses: execute throws ENOSYS.
 *   - No RTC: os.time() returns seconds since boot (NOT epoch).
 *     Same value as os.uptime() until the RTC HAL is implemented.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "lwip/sys.h"      /* sys_now() — TSC-calibrated ms-since-boot */

#include "canboot/env.h"
#include "hal/power.h"

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

#include "error.h"

#if defined(__x86_64__)
#  define CANBOOT_ARCH_STR "x86_64"
#elif defined(__aarch64__)
#  define CANBOOT_ARCH_STR "aarch64"
#else
#  define CANBOOT_ARCH_STR "unknown"
#endif

/* Halt the CPU permanently. Matches the same idiom kernel/kmain.c
 * uses at its fault-return path. */
static void canboot_halt(void) __attribute__((noreturn));
static void canboot_halt(void) {
    for (;;) {
        __asm__ volatile (
#if defined(__x86_64__)
            "cli; hlt"
#elif defined(__aarch64__)
            "msr daifset, #0xf; wfi"
#endif
        );
    }
}

/* Weak power fallbacks: arches without a power driver (e.g. aarch64 today)
 * just halt. x86_64's arch/x86_64/power.c provides the strong ACPI path. */
__attribute__((weak, noreturn)) void canboot_power_off(void) { canboot_halt(); }
__attribute__((weak, noreturn)) void canboot_reboot(void)    { canboot_halt(); }

static f64 seconds_since_boot(void) {
    return (f64)sys_now() / 1000.0;
}

static uint64_t total_usable_bytes(void) {
    const struct boot_info *b = canboot_env_boot_info();
    if (!b) return 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < b->mmap_count && i < CANBOOT_MMAP_MAX; i++) {
        if (b->mmap[i].type == CANBOOT_MMAP_USABLE)
            total += b->mmap[i].length;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Environment table: no environ on bare metal — same null/false shape */
/* host CanDo uses when the underlying call returns no value.          */
/* ------------------------------------------------------------------ */

static int os_getenv(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_null());
    return 1;
}

static int os_setenv(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_bool(false));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Process control                                                    */
/* ------------------------------------------------------------------ */

static int os_execute(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    return canboot_error_throw(vm, "ENOSYS",
        "os.execute: no subprocess support on canboot");
}

static int os_exit(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm; (void)argc; (void)args;
    /* code arg is read but ignored — canboot has no exit code surface
     * to expose to firmware. Halt the kernel; firmware-level reset is
     * up to the operator. */
    canboot_halt();
    return 0; /* unreachable */
}

static int os_poweroff(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm; (void)argc; (void)args;
    canboot_power_off();
    return 0; /* unreachable */
}

static int os_reboot(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm; (void)argc; (void)args;
    canboot_reboot();
    return 0; /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Clocks                                                              */
/* ------------------------------------------------------------------ */

static int os_time(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    /* No RTC yet. Return seconds since boot so scripts that compute
     * time deltas (the dominant use of os.time()) behave correctly. */
    cando_vm_push(vm, cando_number(seconds_since_boot()));
    return 1;
}

static int os_clock(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number(seconds_since_boot()));
    return 1;
}

static int os_uptime(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number(seconds_since_boot()));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

static int os_hostname(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    libutil_push_cstr(vm, "canboot");
    return 1;
}

static int os_tmpdir(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    libutil_push_cstr(vm, "/tmp");
    return 1;
}

static int os_homedir(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    libutil_push_cstr(vm, "/");
    return 1;
}

static int os_arch(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    libutil_push_cstr(vm, CANBOOT_ARCH_STR);
    return 1;
}

static int os_platform(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    libutil_push_cstr(vm, "canboot");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static int os_totalmem(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)total_usable_bytes()));
    return 1;
}

static int os_freemem(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    /* Best-effort. The picolibc 4 MiB static heap is small relative to
     * total memory; until we expose a heap-usage query, report total
     * usable bytes. Scripts that need accurate per-process free can
     * pair this with process.uptime() / app-specific accounting. */
    cando_vm_push(vm, cando_number((f64)total_usable_bytes()));
    return 1;
}

/* os.cpus() -> array of one { model, speed } object.
 * Canboot is single-fiber today; the field is shaped to match host
 * CanDo's surface so scripts that iterate cpus() behave correctly. */
static int os_cpus(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr_obj = cando_bridge_resolve(vm, cando_as_handle(arr_val));

    CdoObject *e = cdo_object_new();

    CdoString *km = cdo_string_intern("model", 5);
    CdoString *ks = cdo_string_intern("speed", 5);
    CdoString *mv = cdo_string_new(CANBOOT_ARCH_STR,
                                    (uint32_t)strlen(CANBOOT_ARCH_STR));

    cdo_object_rawset(e, km, cdo_string_value(mv), FIELD_NONE);
    cdo_object_rawset(e, ks, cdo_number(0), FIELD_NONE);

    cdo_string_release(km);
    cdo_string_release(ks);

    cdo_array_push(arr_obj, cdo_object_value(e));
    cando_vm_push(vm, arr_val);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const LibutilMethodEntry os_methods[] = {
    { "getenv",   os_getenv   },
    { "setenv",   os_setenv   },
    { "execute",  os_execute  },
    { "exit",     os_exit     },
    { "poweroff", os_poweroff },
    { "reboot",   os_reboot   },
    { "time",     os_time     },
    { "clock",    os_clock    },
    { "hostname", os_hostname },
    { "tmpdir",   os_tmpdir   },
    { "homedir",  os_homedir  },
    { "arch",     os_arch     },
    { "platform", os_platform },
    { "uptime",   os_uptime   },
    { "totalmem", os_totalmem },
    { "freemem",  os_freemem  },
    { "cpus",     os_cpus     },
};

void canboot_cando_open_oslib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, os_methods,
                             sizeof(os_methods) / sizeof(os_methods[0]));
    cando_vm_set_global(vm, "os", obj_val, true);
}
