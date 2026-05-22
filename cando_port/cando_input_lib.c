/*
 * cando input module - exposes the milestone-4 HAL input queue to
 * cando scripts as `input.*`:
 *
 *   input.poll()            -> number (ASCII code) or null
 *   input.waitKey(timeoutMs)-> number (ASCII code) or null after timeout
 *   input.flush()           -> drain remaining events (returns count)
 *   input.events()          -> total events received since boot
 *
 * The waitKey timeout uses our TSC-calibrated clock from milestone 6;
 * during the wait we cooperatively pump the HAL input devices so PS/2
 * + virtio-input keep delivering.
 */

#include <stdint.h>
#include <stddef.h>

#include "hal/input.h"

/* Cando audio mixer pump - keep playback going while scripts are
 * stuck inside input.waitKey loops. */
extern void canboot_audio_pump_default(void);

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"

extern uint64_t canboot_tsc_hz(void);
static inline uint64_t rdtsc_local(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#endif
}

static int in_poll(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    int c = hal_input_getc();
    if (c < 0) {
        cando_vm_push(vm, cando_null());
    } else {
        cando_vm_push(vm, cando_number((f64)c));
    }
    return 1;
}

static int in_wait_key(CandoVM *vm, int argc, CandoValue *args) {
    int32_t timeout_ms = (int32_t)libutil_arg_num_at(args, argc, 0, 5000);
    if (timeout_ms < 0) timeout_ms = 0;

    int c = hal_input_getc();
    if (c >= 0) {
        cando_vm_push(vm, cando_number((f64)c));
        return 1;
    }

    uint64_t hz       = canboot_tsc_hz();
    uint64_t deadline = rdtsc_local() + (hz * (uint64_t)timeout_ms) / 1000ull;
    while (rdtsc_local() < deadline) {
        hal_input_pump();
        canboot_audio_pump_default();
        c = hal_input_getc();
        if (c >= 0) {
            cando_vm_push(vm, cando_number((f64)c));
            return 1;
        }
#if defined(__x86_64__)
        __asm__ volatile ("pause");
#elif defined(__aarch64__)
        __asm__ volatile ("yield");
#endif
    }
    cando_vm_push(vm, cando_null());
    return 1;
}

static int in_flush(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    int drained = 0;
    while (hal_input_getc() >= 0) drained++;
    cando_vm_push(vm, cando_number((f64)drained));
    return 1;
}

static int in_events(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)canboot_input_total_events()));
    return 1;
}

static const LibutilMethodEntry input_methods[] = {
    { "poll",    in_poll     },
    { "waitKey", in_wait_key },
    { "flush",   in_flush    },
    { "events",  in_events   },
};

void canboot_cando_open_inputlib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, input_methods,
                             sizeof(input_methods) / sizeof(input_methods[0]));
    cando_vm_set_global(vm, "input", obj_val, true);
}
