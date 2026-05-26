/*
 * cando time module - exposes the kernel monotonic clock as
 *   time.ms()       -> milliseconds since boot (u32)
 *   time.us()       -> microseconds since boot (u64 as f64)
 *   time.now()      -> wall-clock seconds since the Unix epoch (RTC; 0 if none)
 *   time.ticks()    -> raw counter ticks (u64 as f64)
 *   time.ticksHz()  -> counter frequency in Hz (u64 as f64)
 *   time.sleep(ms)  -> busy-wait, pumps net/input devices while waiting
 *
 * Reuses the same x86_64-rdtsc / aarch64-cntvct sources lwIP's
 * sys_arch.c calibrates against; canboot_tsc_hz() and sys_now() are
 * the public entrypoints.
 */

#include <stdint.h>
#include <stddef.h>
#include "lwip/sys.h"
#include "hal/net.h"

/* Cando audio mixer pump - auto-tick from this module so scripts that
 * call time.ms / time.sleep don't need to remember audio.update(). */
extern void canboot_audio_pump_default(void);

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "lib/libutil.h"
#include "lib/object.h"

extern uint64_t canboot_tsc_hz(void);

/* Wall-clock seconds since the Unix epoch. Weak default returns 0 (no
 * wall-clock source); x86_64's CMOS RTC driver (arch/x86_64/rtc.c)
 * overrides it. */
__attribute__((weak)) uint64_t canboot_wall_epoch(void) { return 0; }

static inline uint64_t arch_now(void) {
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

static int t_ms(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    canboot_audio_pump_default();
    cando_vm_push(vm, cando_number((f64)sys_now()));
    return 1;
}

static int t_us(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    uint64_t hz = canboot_tsc_hz();
    uint64_t us = hz ? (arch_now() * 1000000ull) / hz : 0;
    cando_vm_push(vm, cando_number((f64)us));
    return 1;
}

static int t_ticks(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)arch_now()));
    return 1;
}

static int t_ticks_hz(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)canboot_tsc_hz()));
    return 1;
}

/* Wall-clock seconds since the Unix epoch (0 if no RTC). */
static int t_now(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)canboot_wall_epoch()));
    return 1;
}

static int t_sleep(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm;
    uint32_t ms = (uint32_t)libutil_arg_num_at(args, argc, 0, 0);
    uint64_t hz = canboot_tsc_hz();
    uint64_t dl = arch_now() + ((uint64_t)ms * hz) / 1000ull;
    while (arch_now() < dl) {
        hal_net_pump();
        canboot_audio_pump_default();
#if defined(__x86_64__)
        __asm__ volatile ("pause");
#elif defined(__aarch64__)
        __asm__ volatile ("yield");
#endif
    }
    return 0;
}

static const LibutilMethodEntry time_methods[] = {
    { "ms",       t_ms       },
    { "us",       t_us       },
    { "now",      t_now      },
    { "ticks",    t_ticks    },
    { "ticksHz",  t_ticks_hz },
    { "sleep",    t_sleep    },
};

void canboot_cando_open_timelib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, time_methods,
                             sizeof(time_methods) / sizeof(time_methods[0]));
    cando_vm_set_global(vm, "time", obj_val, true);
}
