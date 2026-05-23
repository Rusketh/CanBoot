# Adding a new cando library

A cando library is a C file under `cando_port/` that exposes one or
more functions as methods on a global cando object. New libraries
land in a few hundred lines of code in a single PR.

## The cando_<name>_lib.c shape

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

Three places to add a reference, all in `kernel/m9_candotest.c`:

```c
// Forward declaration:
void canboot_cando_open_thinglib(CandoVM *vm);

// In canboot_m9_candotest(), pick the right milestone block:
canboot_cando_open_thinglib(vm);
printf("milestone NN: thing lib registered\n");
```

Then in `CMakeLists.txt`, add the source file to:

1. The `CANBOOT_KERNEL_COMMON` list (~line 30) — that gets it into
   the x86_64 kernel ELF.
2. The aarch64 EFI source list near the bottom of `CMakeLists.txt`.
3. The x86_64 EFI source list in the middle of `CMakeLists.txt`.

Three lists are unfortunate; once we have a proper module system
this'll collapse. For now, copy the pattern.

## Testing

1. Add a print line in `initramfs/init.cdo` exercising your binding:
   ```cdo
   print("cando thing.thing(7) =", thing.thing(7));
   ```
2. Add a `check 'cando thing.thing(7) = 14'` assertion in the
   relevant runner scripts (`tests/run-qemu-*.sh`).
3. Run them locally:
   ```sh
   cmake --build build && bash scripts/mkiso-bios.sh ...
   bash tests/run-qemu-bios.sh ...
   ```

## Hot-path bindings

If your library does work that other scripts will call from tight
loops (millions of calls per boot), see how `time.ms` is wired in
`cando_port/cando_time_lib.c` — the function calls
`canboot_audio_pump_default()` between iterations so audio playback
continues during `for (...) { time.ms(); ... }` loops. Most libraries
don't need this.

## Examples worth reading

| Lib | What it shows |
|-----|---------------|
| `cando_audio_lib.c` | Multi-state objects (Source pool) + per-instance state + overloaded argc dispatch |
| `cando_image_lib.c` | Vendored single-header decoder; opaque handles; binary input via `libutil_arg_str_at` |
| `cando_crypto_lib.c` | Binary output (returns 32-byte SHA digests as cando strings) |
| `cando_fs_lib.c`     | Dispatching by filesystem type detected at runtime |
| `cando_fmt_lib.c`    | Format helpers; building binary blobs via `fmt.u16le`/`u32le` |

## Picking a name

The cando convention is short lowercase singletons (`fs`, `audio`,
`http`, etc.). Avoid pluralisation; the object IS the library.
