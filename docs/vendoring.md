# Vendoring a third-party library

CanBoot is freestanding — every library it uses is vendored, built
from source, and statically linked into the kernel. No runtime
loading, no shared objects, no package manager.

Two shapes of vendoring exist in the tree:

1. **Single-header libraries** (stb_image, minimp3) — drop the
   header into a `.c` wrapper that defines the IMPLEMENTATION macro.
2. **Submodule libraries with a build system** (picolibc, lwIP,
   Mbed TLS, gnu-efi, ntfs-3g, lwext4, cando) — `git submodule add`,
   then write a canboot-specific compile shim.

This doc walks through both.

## Single-header style (stb / minimp3)

Best fit when the library:

- Lives in one or two header files
- Has no external dependencies beyond `<string.h>` and friends
- Can be configured by `#define` before `#include`

### Steps

```sh
# 1. Add the upstream as a submodule.
git submodule add https://github.com/nothings/stb.git vendor/stb

# 2. Write a thin .c wrapper.
cat > cando_port/vendor_glue/stb/image.c <<'EOF'
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define STBI_NO_STDIO              /* no FILE* I/O */
#define STBI_ASSERT(x) ((void)0)   /* no assert.h */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
EOF

# 3. Add to CMakeLists.txt (twice — main + EFI loops):
#    cando_port/vendor_glue/stb/image.c

# 4. Per-source compile flags pointing at the header:
#    set_source_files_properties(cando_port/vendor_glue/stb/image.c PROPERTIES
#        COMPILE_FLAGS "-I${CMAKE_SOURCE_DIR}/vendor/stb -w")
#
#    (-w silences upstream warnings — we don't police the vendor.)

# 5. If the library needs picolibc functions (malloc, memcpy, strlen),
#    that "just works" — picolibc is already in the link.
```

### Things to watch out for

- **Implementation guards** — set `#define LIB_IMPLEMENTATION` in
  exactly one `.c` file. Doing it in a header that's included from
  multiple TUs triggers duplicate-symbol link errors.
- **Allocator routing** — most single-header libs use `malloc`/`free`
  by default which lands in picolibc's heap. Override with
  `#define LIB_MALLOC(sz) custom_malloc(sz)` etc. if you want a
  custom pool.
- **Compile-time format disables** — stb_image's PNG/JPEG/BMP block
  pulls in ~2 KLOC. Disable formats you don't need (`STBI_NO_GIF`,
  `STBI_NO_PSD`, ...). The footprint difference is measurable in the
  EFI binary size.

## Submodule style (picolibc, lwIP, Mbed TLS, ntfs-3g, lwext4, cando)

When the library is too large to fold into a single header, or has
its own build system that we need to drive.

### Two integration paths

| Path | When |
|------|------|
| **Source-list mode** | Library's source files are listed directly in `CMakeLists.txt` and built as part of canboot's kernel ELF. Used for lwIP, Mbed TLS, ntfs-3g, lwext4. |
| **ExternalProject mode** | Library has its own build system that we invoke. Used for picolibc (meson) and gnu-efi (Makefile). |

### Source-list pattern (lwIP-style)

```sh
# 1. Submodule.
git submodule add https://github.com/lwip-tcpip/lwip.git vendor/lwip

# 2. Write a config header.
mkdir -p net/lwip_port/include
cat > net/lwip_port/include/lwipopts.h <<EOF
#define NO_SYS 1
#define LWIP_NETIF_API 0
#define LWIP_SOCKET    0
/* ... */
EOF

# 3. Write any required port shims.
cat > net/lwip_port/sys_arch.c <<EOF
#include "lwip/sys.h"
u32_t sys_now(void) { /* ... */ }
EOF

# 4. Include the library's source list in CMakeLists.txt.
#    Most projects ship a Filelists.cmake / *.list we can include.
#
#    include(${LWIP_DIR}/src/Filelists.cmake)
#    set(CANBOOT_LWIP_SOURCES ${lwipnoapps_SRCS} net/lwip_port/sys_arch.c)
```

### ExternalProject pattern (picolibc-style)

```sh
# Library has its own build invocation (meson here). Wrap it.
ExternalProject_Add(picolibc_ext
    SOURCE_DIR        ${PICOLIBC_SRC}
    BINARY_DIR        ${PICOLIBC_BUILD}
    PREFIX            ${PICOLIBC_PREFIX}
    CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/scripts/build-picolibc.sh ...
    BUILD_COMMAND     ""    # build inside the configure-script
    INSTALL_COMMAND   ""
    BUILD_BYPRODUCTS  ${PICOLIBC_LIB})

# Then add the .a to the link.
target_link_libraries(canboot-x86_64 PRIVATE ${PICOLIBC_LIB})
```

## Compile-time tricks for freestanding builds

Upstream libraries usually assume hosted environments (FILE*,
fopen, threads, etc.). Strategies for making them freestanding:

### `-include` shim headers

Force-include a header that defines stubs the library expects:

```cmake
set_source_files_properties(${VENDOR_SOURCES} PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_SOURCE_DIR}/cando_port/lib_shim.h -w"
)
```

`cando_port/vendor_glue/ntfs3g/shim.h` is a good example — it provides
`struct hd_geometry`, `LC_ALL`, `ntfs_log_handler` typedefs, etc.
before any libntfs source sees `<linux/...>` headers.

### Macro-overrides at the include site

To swap an upstream symbol for our own at compile time:

```c
#define ntfs_device_default_io_ops canboot_ntfs_device_ops
#include "ntfs/device_io.h"  /* now expands to our symbol */
```

This is how mkntfs picks up our HAL device bridge without modifying
any vendored source files.

### Weak/strong symbol overrides

Provide your own strong definition for a function that the vendored
library declares weak (or vice versa):

```c
__attribute__((weak)) int ntfs_log_handler_outerr(...) { ... }
```

The linker picks the strong definition when present. `hal/audio/audio_stub.c`
uses this to fall back to a no-op backend when no real driver builds
for the current target.

### POSIX stub catch-all

`cando_port/runtime/stubs.c` is the dumping ground for "this library
called `setlocale`, `geteuid`, `openlog`, `random`, …" — write a
no-op or a sensible stub, document why, move on.

## Submodule SHA pinning

Submodule SHAs are pinned to the SHA recorded in the parent repo;
they don't drift on `git pull`. Bump them deliberately:

```sh
cd vendor/lwext4
git fetch
git checkout <new-sha>
cd ../..
git add vendor/lwext4
git commit -m "lwext4: bump to <new-sha>"
```

Always run the full smoke test after a vendor bump — even
"compatible" library updates have surprised us in the past
(picolibc 1.8 -> 1.9 changed default printf format options).

## License tracking

Add the new vendored library to the table in the top-level README's
License section + the licenses sub-table. The CI release workflow
doesn't currently verify licenses; that's a human review step before
each `v*` tag.
