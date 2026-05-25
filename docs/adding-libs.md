# Adding a new cando library

A cando library is a C file under `cando_port/lib/` that exposes one or
more functions as methods on a global cando object. New libraries
land in a few hundred lines of code in a single PR.

## The cando_port/lib/&lt;name&gt;.c shape

```c
#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

static int f_thing(CandoVM *vm, int argc, CandoValue *args) {
    int x = (int)libutil_arg_num_at(args, argc, 0, 0);
    cando_vm_push(vm, cando_number((double)(x * 2)));
    return 1;   // one value pushed
}

static const LibutilMethodEntry methods[] = {
    { "thing", f_thing },
};

void canboot_cando_open_thinglib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, methods,
                             sizeof(methods) / sizeof(methods[0]));
    cando_vm_set_global(vm, "thing", obj_val, true);
}
```

The script side then has `thing.thing(N)` available globally.

## Argument helpers

| Helper | Notes |
|--------|-------|
| `libutil_arg_num_at(args, argc, idx, default)`  | f64 number; returns `default` if missing/non-number |
| `libutil_arg_cstr_at(args, argc, idx)`          | NUL-terminated C string; `NULL` if missing/non-string. Truncates on embedded NULs. |
| `libutil_arg_str_at(args, argc, idx)`           | The full `CandoString *` with `->length` field. **Use this for binary data.** |

`libutil_arg_cstr_at` is convenient but unsafe for any input that
might contain bytes from `fs.read`. Default to `libutil_arg_str_at`
unless you know the argument is text.

## Pushing return values

`cando_vm_push(vm, ...)` followed by `return N;` where N is the count
of values pushed (almost always 1).

Constructors:

```c
cando_vm_push(vm, cando_null());
cando_vm_push(vm, cando_bool(true));
cando_vm_push(vm, cando_number(42.0));
cando_vm_push(vm, cando_string_value(cando_string_new(buf, len)));
```

For strings, `cando_string_new(ptr, length)` is the binary-safe form
(takes an explicit length). The `length` is preserved on the cando
side — embedded NULs are fine.

## Registering the library

Two references in `tests/selftest/cando.c` — a forward declaration and
a call inside `cando_selftest()`:

```c
// Forward declaration (near the top, with the others):
void canboot_cando_open_thinglib(CandoVM *vm);

// In cando_selftest(), in the appropriate milestone block:
canboot_cando_open_thinglib(vm);
printf("selftest: thing lib registered\n");
```

Then add the source file to the **single** `CANBOOT_PORTABLE_SOURCES`
list in `cmake/sources.cmake` (with the other `cando_port/lib/*.c`
entries):

```cmake
set(CANBOOT_PORTABLE_SOURCES
    ...
    cando_port/lib/thing.c
    ...)
```

That one list feeds every target — the BIOS/x86_64 kernel ELF (via
`CANBOOT_KERNEL_COMMON`) and both UEFI loaders (x86_64 inherits
`CANBOOT_KERNEL_COMMON`; aarch64 pulls `CANBOOT_PORTABLE_SOURCES`
directly). No per-build duplication.

## Testing

1. Add a print line in `initramfs/init.cdo` exercising your binding:
   ```cdo
   print("cando thing.thing(7) =", thing.thing(7));
   ```
2. Add a `check 'cando thing.thing(7) = 14'` assertion in the
   relevant runner scripts (`tests/run-qemu-*.sh`).
3. Run them locally:
   ```sh
   cmake --build build && bash scripts/mkiso/bios.sh ...
   bash tests/run-qemu-bios.sh ...
   ```

## Hot-path bindings

If your library does work that other scripts will call from tight
loops (millions of calls per boot), see how `time.ms` is wired in
`cando_port/lib/time.c` — the function calls
`canboot_audio_pump_default()` between iterations so audio playback
continues during `for (...) { time.ms(); ... }` loops. Most libraries
don't need this.

## Examples worth reading

| Lib | What it shows |
|-----|---------------|
| `cando_port/lib/audio.c`  | Multi-state objects (Source pool) + per-instance state + overloaded argc dispatch |
| `cando_port/lib/image.c`  | Vendored single-header decoder; opaque handles; binary input via `libutil_arg_str_at` |
| `cando_port/lib/crypto.c` | Binary output (returns 32-byte SHA digests as cando strings) |
| `cando_port/lib/fs.c`     | Dispatching by filesystem type detected at runtime |
| `cando_port/lib/fmt.c`    | Format helpers; building binary blobs via `fmt.u16le`/`u32le` |

## Picking a name

The cando convention is short lowercase singletons (`fs`, `audio`,
`http`, etc.). Avoid pluralisation; the object IS the library.
