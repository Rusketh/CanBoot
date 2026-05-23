cmake_minimum_required(VERSION 3.20)

project(canboot LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT DEFINED CANBOOT_ARCH)
    set(CANBOOT_ARCH "x86_64" CACHE STRING "Target architecture")
endif()

set(CANBOOT_SUPPORTED_ARCHS x86_64 aarch64)
if(NOT CANBOOT_ARCH IN_LIST CANBOOT_SUPPORTED_ARCHS)
    message(FATAL_ERROR "Unsupported CANBOOT_ARCH '${CANBOOT_ARCH}'. Supported: ${CANBOOT_SUPPORTED_ARCHS}")
endif()

# ---------------------------------------------------------------------------
# Source-list variables (CANBOOT_KERNEL_COMMON / CANBOOT_LWIP_SOURCES /
# CANBOOT_CANDO_SOURCES / CANBOOT_NTFS3G_SOURCES /
# CANBOOT_NTFS3G_MKFS_SOURCES / CANBOOT_LWEXT4_SOURCES) and the
# LWIP_DIR / CANDO_DIR / NTFS3G_DIR / LWEXT4_DIR locations they imply.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/sources.cmake)

# ---------------------------------------------------------------------------
# picolibc: cross-built per-arch via ExternalProject. Exports
# PICOLIBC_INCLUDE, PICOLIBC_LIB, target picolibc_ext.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/picolibc.cmake)

# ---------------------------------------------------------------------------
# Kernel ELF target (BIOS path on x86_64, direct-kernel on aarch64).
# ---------------------------------------------------------------------------
set(KERNEL_NAME "canboot-${CANBOOT_ARCH}")
set(KERNEL_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/linker/kernel.${CANBOOT_ARCH}.ld")

if(CANBOOT_ARCH STREQUAL "x86_64")
    set(KERNEL_SOURCES
        arch/x86_64/bootstrap.S
        arch/x86_64/bios_entry.c
        arch/x86_64/mb2_parse.c
        boot/multiboot2/header.S
        ${CANBOOT_KERNEL_COMMON}
        ${CANBOOT_LWIP_SOURCES}
        ${CANBOOT_CANDO_SOURCES}
        ${CANBOOT_NTFS3G_SOURCES}
        ${CANBOOT_NTFS3G_MKFS_SOURCES}
        ${CANBOOT_LWEXT4_SOURCES}
        hal/audio/intel_hda.c
    )
elseif(CANBOOT_ARCH STREQUAL "aarch64")
    # aarch64 direct-kernel build: bootstrap + entry trampoline + PL011
    # console + the same kmain body as x86_64, with the aarch64-specific
    # HAL drivers (PL011, virtio-gpu, virtio-snd) compiled in.
    set(KERNEL_SOURCES
        arch/aarch64/bootstrap.S
        arch/aarch64/aarch64_entry.c
        arch/aarch64/fdt.c
        kernel/kmain_aarch64.c
        kernel/env.c
        kernel/fb.c
        tests/selftest/runtime.c
        hal/console/serial_aarch64.c
        hal/input/input_stub_aarch64.c
        rt/picolibc_port/syscalls.c
        rt/pthread_stub/pthread.c
    )
endif()

add_executable(${KERNEL_NAME} ${KERNEL_SOURCES})

# ---------------------------------------------------------------------------
# Per-source compile flags for vendored libraries (libntfs-3g, mkntfs,
# lwext4, stb_image, minimp3). Applied here in top-level scope so the
# properties stick to the source-file paths regardless of which target
# pulls them in.
# ---------------------------------------------------------------------------

# libntfs-3g: pre-include our shim header, point at the canboot
# config.h, suppress upstream warnings we have no business policing,
# and bypass HAVE_CONFIG_H gating. ntfs3g io.c only needs the include
# path - the shim picks up the canboot config.h transitively.
set_source_files_properties(${CANBOOT_NTFS3G_SOURCES} PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/shim.h -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g -I${CMAKE_SOURCE_DIR}/cando_port/shims -DHAVE_CONFIG_H=1 -w"
)

# mkntfs.c and friends need:
#  - to NOT see NO_NTFS_DEVICE_DEFAULT_IO_OPS (since mkntfs.c #errors
#    out when that's set - it needs the default ops table to allocate
#    the device. We provide our HAL ops under that name in
#    cando_port/vendor_glue/ntfs3g/io.c).
#  - main renamed to mkntfs_main_canboot so the kernel's real entry
#    is unaffected.
#  - getopt.h provided by our shim - cando_port/shims/getopt.h.
#  - -fcommon so tentative defs across mkntfs.c + sd.c don't conflict
#    (the upstream relies on the pre-C99 behaviour).
set_source_files_properties(${CANBOOT_NTFS3G_MKFS_SOURCES} PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/shim.h -include ${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/mkntfs_main_rename.h -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/ntfsprogs -I${CMAKE_SOURCE_DIR}/cando_port/shims -DCANBOOT_MKNTFS=1 -fno-plt -fvisibility=hidden -fcommon -DHAVE_CONFIG_H=1 -w"
)
set_source_files_properties(cando_port/vendor_glue/ntfs3g/io.c cando_port/vendor_glue/ntfs3g/glue.c PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/shim.h -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g -DHAVE_CONFIG_H=1 -w"
)

# lwext4: CONFIG_USE_DEFAULT_CFG=1 picks up the vendored
# ext4_config.h defaults; CONFIG_DEBUG_PRINTF=0 silences per-block
# trace prints that would otherwise flood the serial console.
set_source_files_properties(${CANBOOT_LWEXT4_SOURCES} cando_port/vendor_glue/lwext4/io.c cando_port/vendor_glue/lwext4/glue.c PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_SOURCE_DIR}/vendor/lwext4/include -I${CMAKE_SOURCE_DIR}/vendor/lwext4_canboot -I${CMAKE_SOURCE_DIR}/cando_port -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_DEBUG_PRINTF=0 -w"
)

# stb_image's single-header body. -w suppresses the noisy warnings the
# vendored decoder triggers under -Wall.
set_source_files_properties(cando_port/vendor_glue/stb/image.c PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_SOURCE_DIR}/vendor/stb -w"
)

# minimp3's single-header body. Same pattern as stb_image.
set_source_files_properties(cando_port/vendor_glue/minimp3/decoder.c PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_SOURCE_DIR}/vendor/minimp3 -w"
)

# ---------------------------------------------------------------------------
# Per-arch kernel target wiring (compile options, include paths,
# link options + libraries).
# ---------------------------------------------------------------------------
if(CANBOOT_ARCH STREQUAL "x86_64")
    add_dependencies(${KERNEL_NAME} picolibc_ext)

    set_source_files_properties(
        arch/x86_64/bootstrap.S
        arch/x86_64/idt_stubs.S
        boot/multiboot2/header.S
        PROPERTIES LANGUAGE ASM
    )

    target_include_directories(${KERNEL_NAME} BEFORE PRIVATE
        cando_port/shims
    )

    target_include_directories(${KERNEL_NAME} PRIVATE
        hal/include
        kernel/include
        rt/pthread_stub/include
        net/lwip_port/include
        net/mbedtls_port/include
        net/mbedtls_port
        ${CMAKE_SOURCE_DIR}
        ${LWIP_DIR}/src/include
        ${PICOLIBC_INCLUDE}
        ${CMAKE_SOURCE_DIR}/vendor/mbedtls/include
        ${CANDO_DIR}/include
        ${CANDO_DIR}/source
    )

    target_compile_options(${KERNEL_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
        $<$<COMPILE_LANGUAGE:C>:-fno-stack-protector>
        $<$<COMPILE_LANGUAGE:C>:-fno-stack-check>
        $<$<COMPILE_LANGUAGE:C>:-fno-pic>
        $<$<COMPILE_LANGUAGE:C>:-fno-pie>
        $<$<COMPILE_LANGUAGE:C>:-mno-red-zone>
        $<$<COMPILE_LANGUAGE:C>:-mcmodel=kernel>
        $<$<COMPILE_LANGUAGE:C>:-fcf-protection=none>
        -Wall
        -Wextra
    )

    target_compile_options(${KERNEL_NAME} PRIVATE
        -D_GNU_SOURCE
        -DSSIZE_MAX=__LONG_MAX__
        "SHELL:-iquote ${CANDO_DIR}/source"
        "SHELL:-iquote ${CANDO_DIR}/source/core"
        "SHELL:-iquote ${CANDO_DIR}/source/object"
        "SHELL:-iquote ${CANDO_DIR}/source/parser"
        "SHELL:-iquote ${CANDO_DIR}/source/vm"
        "SHELL:-iquote ${CANDO_DIR}/source/jit"
        "SHELL:-iquote ${CANDO_DIR}/source/lib"
    )

    target_link_options(${KERNEL_NAME} PRIVATE
        -nostdlib
        -no-pie
        -static
        -Wl,--build-id=none
        -Wl,-T,${KERNEL_LINKER_SCRIPT}
        -Wl,-z,noexecstack
        -Wl,-z,max-page-size=0x1000
    )

    target_link_libraries(${KERNEL_NAME} PRIVATE
        -Wl,--start-group
        mbedtls mbedx509 mbedcrypto
        ${PICOLIBC_LIB}
        -Wl,--end-group
        -lgcc
    )

elseif(CANBOOT_ARCH STREQUAL "aarch64")
    add_dependencies(${KERNEL_NAME} picolibc_ext)

    set_source_files_properties(
        arch/aarch64/bootstrap.S
        PROPERTIES LANGUAGE ASM
    )

    target_include_directories(${KERNEL_NAME} PRIVATE
        hal/include
        kernel/include
        rt/pthread_stub/include
        ${CMAKE_SOURCE_DIR}
        ${PICOLIBC_INCLUDE}
    )

    target_compile_options(${KERNEL_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
        $<$<COMPILE_LANGUAGE:C>:-fno-stack-protector>
        $<$<COMPILE_LANGUAGE:C>:-fno-stack-check>
        $<$<COMPILE_LANGUAGE:C>:-fno-pic>
        $<$<COMPILE_LANGUAGE:C>:-fno-pie>
        $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>
        -Wall
        -Wextra
    )

    target_link_options(${KERNEL_NAME} PRIVATE
        -nostdlib
        -no-pie
        -static
        -Wl,--build-id=none
        -Wl,-T,${KERNEL_LINKER_SCRIPT}
        -Wl,-z,noexecstack
        -Wl,-z,max-page-size=0x1000
    )

    target_link_libraries(${KERNEL_NAME} PRIVATE
        ${PICOLIBC_LIB}
        -lgcc
    )
endif()

set_target_properties(${KERNEL_NAME} PROPERTIES
    SUFFIX ".elf"
    LINK_DEPENDS ${KERNEL_LINKER_SCRIPT}
)

# On aarch64 we also emit a raw .bin via objcopy. The bootstrap.S
# starts with a 64-byte Linux ARM64 boot header (b _boot_entry + magic
# + ...), which makes QEMU's -kernel path detect us as a "Linux"
# kernel and run its boot stub - that stub sets x0 = FDT phys at
# entry. Plain ELF -kernel skips the stub and leaves x0 = 0, so the
# raw image is the canonical artifact for the direct-kernel boot path.
if(CANBOOT_ARCH STREQUAL "aarch64")
    set(KERNEL_BIN "${CMAKE_BINARY_DIR}/${KERNEL_NAME}.bin")
    add_custom_command(
        OUTPUT ${KERNEL_BIN}
        COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${KERNEL_NAME}> ${KERNEL_BIN}
        DEPENDS ${KERNEL_NAME}
        COMMENT "BIN ${KERNEL_NAME}.bin"
        VERBATIM
    )
    add_custom_target(canboot-aarch64-bin ALL DEPENDS ${KERNEL_BIN})
endif()

add_custom_target(canboot-info ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Kernel ELF: $<TARGET_FILE:${KERNEL_NAME}>"
    DEPENDS ${KERNEL_NAME}
)

# ---------------------------------------------------------------------------
# Mbed TLS 3.6.x: vendored at vendor/mbedtls, brought in with our
# freestanding cross flags pre-applied.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/mbedtls.cmake)

# ---------------------------------------------------------------------------
# UEFI loader (per-arch).
# ---------------------------------------------------------------------------
if(CANBOOT_ARCH STREQUAL "x86_64")
    include(${CMAKE_SOURCE_DIR}/cmake/uefi_x86_64.cmake)
elseif(CANBOOT_ARCH STREQUAL "aarch64")
    include(${CMAKE_SOURCE_DIR}/cmake/uefi_aarch64.cmake)
endif()
