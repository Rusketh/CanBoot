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

# Sources shared between the BIOS kernel and the UEFI loader. These get
# compiled twice with different flags (kernel-model vs PIC EFI) and linked
# into each target separately.
set(CANBOOT_KERNEL_COMMON
    arch/x86_64/idt.c
    arch/x86_64/idt_stubs.S
    kernel/kmain.c
    kernel/env.c
    kernel/fb.c
    kernel/m5_selftest.c
    kernel/m6_nettest.c
    kernel/m7_tlstest.c
    kernel/m8_disktest.c
    kernel/m9_candotest.c
    kernel/canboot_test_ca.c
    cando_port/cando_stubs.c
    cando_port/cando_display_lib.c
    cando_port/cando_input_lib.c
    cando_port/cando_time_lib.c
    cando_port/cando_file_lib.c
    cando_port/cando_net_lib.c
    cando_port/cando_tls_lib.c
    cando_port/cando_random_lib.c
    cando_port/cando_crypto_lib.c
    cando_port/cando_encoding_lib.c
    cando_port/cando_log_lib.c
    cando_port/cando_env_lib.c
    cando_port/cando_url_lib.c
    cando_port/cando_http_lib.c
    cando_port/cando_disk_lib.c
    cando_port/cando_pci_lib.c
    cando_port/cando_fb_lib.c
    cando_port/cando_fmt_lib.c
    cando_port/cando_partition_lib.c
    cando_port/cando_fs_lib.c
    cando_port/ntfs3g_canboot_io.c
    cando_port/ntfs3g_canboot_glue.c
    fs/partition.c
    fs/ntfs.c
    fs/iso9660.c
    fs/fat32.c
    hal/disk/disk.c
    hal/disk/virtio_blk.c
    hal/disk/ahci.c
    hal/display/display.c
    hal/console/serial_x86.c
    hal/input/input_queue.c
    hal/input/ps2.c
    hal/input/virtio_input.c
    hal/net/virtio_net.c
    hal/pci/pci_x86.c
    hal/virtio/virtio_pci.c
    net/lwip_port/sys_arch.c
    net/mbedtls_port/entropy.c
    net/mbedtls_port/inet_pton.c
    net/mbedtls_port/lwip_bio.c
    rt/picolibc_port/syscalls.c
    rt/pthread_stub/pthread.c
)

# lwIP source files brought in via the vendored Filelists.cmake. The list
# below mirrors what kernel + UEFI link in; it's appended to the common
# source set for each target.
set(LWIP_DIR ${CMAKE_SOURCE_DIR}/vendor/lwip)
include(${LWIP_DIR}/src/Filelists.cmake)
set(CANBOOT_LWIP_SOURCES
    ${lwipcore_SRCS}
    ${lwipcore4_SRCS}
    ${lwipnetif_SRCS}
)

# CanDo language vendor at vendor/cando. We compile a curated subset of
# its source files directly into the kernel rather than going through
# its CMakeLists, which would pull in find_package(OpenSSL) - we don't
# ship OpenSSL on bare metal. The dropped SSL/socket/HTTP libs are
# stubbed via cando_port/cando_stubs.c (no-op register hooks); real
# Mbed TLS + lwIP-backed bindings land in a later milestone.
set(CANDO_DIR ${CMAKE_SOURCE_DIR}/vendor/cando)
set(CANBOOT_CANDO_SOURCES
    ${CANDO_DIR}/source/cando_lib.c
    ${CANDO_DIR}/source/natives.c
    ${CANDO_DIR}/source/core/common.c
    ${CANDO_DIR}/source/core/value.c
    ${CANDO_DIR}/source/core/lock.c
    ${CANDO_DIR}/source/core/handle.c
    ${CANDO_DIR}/source/core/memory.c
    ${CANDO_DIR}/source/core/thread_platform.c
    ${CANDO_DIR}/source/object/string.c
    ${CANDO_DIR}/source/object/value.c
    ${CANDO_DIR}/source/object/object.c
    ${CANDO_DIR}/source/object/array.c
    ${CANDO_DIR}/source/object/function.c
    ${CANDO_DIR}/source/object/class.c
    ${CANDO_DIR}/source/object/thread.c
    ${CANDO_DIR}/source/parser/lexer.c
    ${CANDO_DIR}/source/parser/parser.c
    ${CANDO_DIR}/source/vm/opcodes.c
    ${CANDO_DIR}/source/vm/chunk.c
    ${CANDO_DIR}/source/vm/bridge.c
    ${CANDO_DIR}/source/vm/vm.c
    ${CANDO_DIR}/source/vm/debug.c
    ${CANDO_DIR}/source/jit/ir.c
    ${CANDO_DIR}/source/jit/hot.c
    ${CANDO_DIR}/source/jit/mcode.c
    ${CANDO_DIR}/source/jit/codegen.c
    ${CANDO_DIR}/source/jit/jit.c
    ${CANDO_DIR}/source/lib/gc.c
    ${CANDO_DIR}/source/lib/jit.c
    ${CANDO_DIR}/source/lib/app.c
    ${CANDO_DIR}/source/lib/math.c
    # ${CANDO_DIR}/source/lib/file.c   -- replaced by cando_port/cando_file_lib.c
    ${CANDO_DIR}/source/lib/eval.c
    ${CANDO_DIR}/source/lib/string.c
    ${CANDO_DIR}/source/lib/libutil.c
    ${CANDO_DIR}/source/lib/include.c
    ${CANDO_DIR}/source/lib/json.c
    ${CANDO_DIR}/source/lib/csv.c
    ${CANDO_DIR}/source/lib/yaml.c
    ${CANDO_DIR}/source/lib/thread.c
    ${CANDO_DIR}/source/lib/os.c
    ${CANDO_DIR}/source/lib/datetime.c
    ${CANDO_DIR}/source/lib/array.c
    ${CANDO_DIR}/source/lib/console.c
    ${CANDO_DIR}/source/lib/console_term.c
    ${CANDO_DIR}/source/lib/console_input.c
    ${CANDO_DIR}/source/lib/console_events.c
    ${CANDO_DIR}/source/lib/console_lineedit.c
    ${CANDO_DIR}/source/lib/console_dispatch.c
    ${CANDO_DIR}/source/lib/process.c
    # ${CANDO_DIR}/source/lib/net.c    -- replaced by cando_port/cando_net_lib.c
    ${CANDO_DIR}/source/lib/object.c
    ${CANDO_DIR}/source/lib/meta.c
    ${CANDO_DIR}/source/lib/stream.c
)

# ---------------------------------------------------------------------------
# ntfs-3g library sources. Vendored under vendor/ntfs-3g, compiled
# against vendor/ntfs-3g_canboot/config.h with the pre-include shim
# at cando_port/ntfs3g_canboot_shim.h. Skips win32_io.c (Windows-only).
# unix_io.c is included for its declarations; NO_NTFS_DEVICE_DEFAULT_IO_OPS
# in config.h elides the default ops table - we register our own via
# cando_port/ntfs3g_canboot_io.c.
# ---------------------------------------------------------------------------
set(NTFS3G_DIR ${CMAKE_SOURCE_DIR}/vendor/ntfs-3g)
set(CANBOOT_NTFS3G_SOURCES
    ${NTFS3G_DIR}/libntfs-3g/acls.c
    ${NTFS3G_DIR}/libntfs-3g/attrib.c
    ${NTFS3G_DIR}/libntfs-3g/attrlist.c
    ${NTFS3G_DIR}/libntfs-3g/bitmap.c
    ${NTFS3G_DIR}/libntfs-3g/bootsect.c
    ${NTFS3G_DIR}/libntfs-3g/cache.c
    ${NTFS3G_DIR}/libntfs-3g/collate.c
    ${NTFS3G_DIR}/libntfs-3g/compat.c
    ${NTFS3G_DIR}/libntfs-3g/compress.c
    ${NTFS3G_DIR}/libntfs-3g/debug.c
    ${NTFS3G_DIR}/libntfs-3g/device.c
    ${NTFS3G_DIR}/libntfs-3g/dir.c
    ${NTFS3G_DIR}/libntfs-3g/ea.c
    ${NTFS3G_DIR}/libntfs-3g/efs.c
    ${NTFS3G_DIR}/libntfs-3g/index.c
    ${NTFS3G_DIR}/libntfs-3g/inode.c
    ${NTFS3G_DIR}/libntfs-3g/ioctl.c
    ${NTFS3G_DIR}/libntfs-3g/lcnalloc.c
    ${NTFS3G_DIR}/libntfs-3g/logfile.c
    ${NTFS3G_DIR}/libntfs-3g/logging.c
    ${NTFS3G_DIR}/libntfs-3g/mft.c
    ${NTFS3G_DIR}/libntfs-3g/misc.c
    ${NTFS3G_DIR}/libntfs-3g/mst.c
    ${NTFS3G_DIR}/libntfs-3g/object_id.c
    ${NTFS3G_DIR}/libntfs-3g/realpath.c
    ${NTFS3G_DIR}/libntfs-3g/reparse.c
    ${NTFS3G_DIR}/libntfs-3g/runlist.c
    ${NTFS3G_DIR}/libntfs-3g/security.c
    ${NTFS3G_DIR}/libntfs-3g/unistr.c
    ${NTFS3G_DIR}/libntfs-3g/unix_io.c
    ${NTFS3G_DIR}/libntfs-3g/volume.c
    ${NTFS3G_DIR}/libntfs-3g/xattrs.c
)

# ntfsprogs/mkntfs.c provides format. We pull in the format driver
# and a couple of helpers (sd, boot, attrdef) that bake security
# descriptors / boot sector code / attribute defs into the new volume.
set(CANBOOT_NTFS3G_MKFS_SOURCES
    ${NTFS3G_DIR}/ntfsprogs/mkntfs.c
    ${NTFS3G_DIR}/ntfsprogs/sd.c
    ${NTFS3G_DIR}/ntfsprogs/boot.c
    ${NTFS3G_DIR}/ntfsprogs/attrdef.c
    ${NTFS3G_DIR}/ntfsprogs/utils.c
)

# ---------------------------------------------------------------------------
# picolibc - cross-built for the current arch via ExternalProject. The
# x86_64 build has used this since milestone 5; aarch64 m5 hooks into
# the same machinery via cmake/picolibc-aarch64.cross.in.
# ---------------------------------------------------------------------------
if(CANBOOT_ARCH STREQUAL "x86_64" OR CANBOOT_ARCH STREQUAL "aarch64")
    include(ExternalProject)

    set(PICOLIBC_SRC       "${CMAKE_SOURCE_DIR}/vendor/picolibc")
    set(PICOLIBC_PREFIX    "${CMAKE_BINARY_DIR}/picolibc")
    set(PICOLIBC_BUILD     "${PICOLIBC_PREFIX}/build")
    set(PICOLIBC_INSTALL   "${PICOLIBC_PREFIX}/install")
    set(PICOLIBC_CROSS_IN  "${CMAKE_SOURCE_DIR}/cmake/picolibc-${CANBOOT_ARCH}.cross.in")
    set(PICOLIBC_CROSS     "${PICOLIBC_PREFIX}/picolibc-${CANBOOT_ARCH}.cross")
    set(PICOLIBC_INCLUDE   "${PICOLIBC_INSTALL}/include")
    set(PICOLIBC_LIB       "${PICOLIBC_INSTALL}/lib/libc.a")

    configure_file(${PICOLIBC_CROSS_IN} ${PICOLIBC_CROSS} @ONLY)

    ExternalProject_Add(picolibc_ext
        SOURCE_DIR        ${PICOLIBC_SRC}
        BINARY_DIR        ${PICOLIBC_BUILD}
        PREFIX            ${PICOLIBC_PREFIX}
        CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/scripts/build-picolibc.sh
                              ${PICOLIBC_SRC}
                              ${PICOLIBC_BUILD}
                              ${PICOLIBC_INSTALL}
                              ${PICOLIBC_CROSS}
        BUILD_COMMAND     ""
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS  ${PICOLIBC_LIB}
        USES_TERMINAL_BUILD TRUE
    )
endif()

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
    )
elseif(CANBOOT_ARCH STREQUAL "aarch64")
    # Minimal first-cut aarch64 build: bootstrap + entry trampoline +
    # PL011 console + a stub kmain that mirrors x86_64's milestone-1
    # "ok on serial" deliverable. Subsequent aarch64 milestones port
    # picolibc / lwIP / cando / etc. one PR at a time.
    set(KERNEL_SOURCES
        arch/aarch64/bootstrap.S
        arch/aarch64/aarch64_entry.c
        arch/aarch64/fdt.c
        kernel/kmain_aarch64.c
        kernel/env.c
        kernel/fb.c
        kernel/m5_selftest.c
        hal/console/serial_aarch64.c
        hal/input/input_stub_aarch64.c
        rt/picolibc_port/syscalls.c
        rt/pthread_stub/pthread.c
    )
else()
    message(FATAL_ERROR "Arch ${CANBOOT_ARCH} bring-up lands in a later milestone")
endif()

add_executable(${KERNEL_NAME} ${KERNEL_SOURCES})

# libntfs-3g per-source compile flags: pre-include our shim header,
# point at the canboot config.h, suppress upstream warnings we have
# no business policing, and bypass HAVE_CONFIG_H gating. ntfs3g_canboot_io.c
# only needs the include path - the shim picks up the canboot config.h
# transitively. Applied to both the x86_64 BIOS+UEFI and aarch64 EFI
# targets because the source set is shared.
set_source_files_properties(${CANBOOT_NTFS3G_SOURCES} PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_SOURCE_DIR}/cando_port/ntfs3g_canboot_shim.h -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g -I${CMAKE_SOURCE_DIR}/cando_port/shims -DHAVE_CONFIG_H=1 -w"
)

# mkntfs.c and friends need:
#  - to NOT see NO_NTFS_DEVICE_DEFAULT_IO_OPS (since mkntfs.c #errors
#    out when that's set - it needs the default ops table to allocate
#    the device. We provide our HAL ops under that name in
#    cando_port/ntfs3g_canboot_io.c).
#  - main renamed to mkntfs_main_canboot so the kernel's real entry
#    is unaffected.
#  - getopt.h provided by our shim - cando_port/shims/getopt.h.
#  - -fcommon so tentative defs across mkntfs.c + sd.c don't conflict
#    (the upstream relies on the pre-C99 behaviour).
set_source_files_properties(${CANBOOT_NTFS3G_MKFS_SOURCES} PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_SOURCE_DIR}/cando_port/ntfs3g_canboot_shim.h -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/ntfsprogs -I${CMAKE_SOURCE_DIR}/cando_port/shims -DCANBOOT_MKNTFS=1 -Dmain=mkntfs_main_canboot -fcommon -DHAVE_CONFIG_H=1 -w"
)
set_source_files_properties(cando_port/ntfs3g_canboot_io.c cando_port/ntfs3g_canboot_glue.c PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_SOURCE_DIR}/cando_port/ntfs3g_canboot_shim.h -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g -DHAVE_CONFIG_H=1 -w"
)

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

# On aarch64 we also emit a raw .bin via objcopy. The bootstrap.S starts
# with a 64-byte Linux ARM64 boot header (b _boot_entry + magic + ...),
# which makes QEMU's -kernel path detect us as a "Linux" kernel and run
# its boot stub - that stub is what sets x0 = FDT phys at entry. Plain
# ELF -kernel skips the stub and leaves x0 = 0, so the raw image is the
# canonical artifact for the direct-kernel boot path.
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
# Mbed TLS 3.6.x: vendored at vendor/mbedtls and brought in via
# add_subdirectory(EXCLUDE_FROM_ALL). We pre-apply our freestanding cross
# flags through add_compile_options() so they reach Mbed TLS's targets
# (mbedtls / mbedx509 / tfpsacrypto) automatically. -Werror is disabled
# (MBEDTLS_FATAL_WARNINGS=OFF) because freestanding flags trip a few of
# Mbed TLS's own warnings that we have no business fixing.
# ---------------------------------------------------------------------------
if(CANBOOT_ARCH STREQUAL "x86_64" OR CANBOOT_ARCH STREQUAL "aarch64")
    set(MBEDTLS_DIR ${CMAKE_SOURCE_DIR}/vendor/mbedtls)
    set(MBEDTLS_CONFIG_FILE "" CACHE STRING "" FORCE)
    set(MBEDTLS_USER_CONFIG_FILE
        "${CMAKE_SOURCE_DIR}/net/mbedtls_port/include/canboot_mbedtls_user_config.h"
        CACHE STRING "" FORCE)

    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    set(GEN_FILES ON CACHE BOOL "" FORCE)

    # -D_Thread_local= flattens cando's four _Thread_local statics
    # into plain globals (mbedtls doesn't use TLS itself in our config
    # but we keep the same flags so the EFI link sees identical TUs).
    if(CANBOOT_ARCH STREQUAL "x86_64")
        add_compile_options(
            -ffreestanding -fno-stack-protector -fno-stack-check
            -mno-red-zone -fPIC -fcf-protection=none
            -D_Thread_local= -D__thread=
        )
    else()
        # On aarch64-cross gcc, /usr/include/limits.h pulls in
        # bits/libc-header-start.h which only exists if
        # libc6-dev-arm64-cross is installed. picolibc has the same
        # headers in a self-contained form; we route mbedtls through
        # them by prepending -I (BEFORE without SYSTEM emits a plain
        # -I, which gcc searches *before* its internal include-fixed
        # tree - the only way to win against /usr/include/limits.h).
        include_directories(BEFORE ${PICOLIBC_INCLUDE})
        add_compile_options(
            -ffreestanding -fno-stack-protector -fno-stack-check
            -fPIC
            -D_Thread_local= -D__thread=
        )
    endif()
    add_subdirectory(${MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)

    # On aarch64 mbedtls includes picolibc's <limits.h> (its own pulls
    # /usr/include/limits.h -> bits/libc-header-start.h which only
    # exists with libc6-dev-arm64-cross). Force the mbedtls targets to
    # wait for picolibc_ext so the install/include tree exists before
    # any mbedtls source compiles.
    if(CANBOOT_ARCH STREQUAL "aarch64")
        foreach(_t mbedtls mbedx509 mbedcrypto everest p256m)
            if(TARGET ${_t})
                add_dependencies(${_t} picolibc_ext)
            endif()
        endforeach()
    endif()
endif()
# kernel/ sources with PIC + short-wchar + EFI include path, links them
# with gnu-efi's crt0 and elf_x86_64_efi.lds, then objcopies to a PE32+
# EFI application. Calls into the same kmain as the BIOS kernel.
# ---------------------------------------------------------------------------
if(CANBOOT_ARCH STREQUAL "x86_64")
    find_path(GNUEFI_INCLUDE_DIR efi.h
        HINTS /usr/include/efi /usr/local/include/efi
    )
    find_path(GNUEFI_ARCH_INCLUDE_DIR efibind.h
        HINTS /usr/include/efi/x86_64 /usr/local/include/efi/x86_64
    )
    find_file(GNUEFI_CRT0 crt0-efi-x86_64.o
        HINTS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
    )
    find_file(GNUEFI_LDS elf_x86_64_efi.lds
        HINTS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
    )
    find_library(GNUEFI_LIB gnuefi
        HINTS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
    )
    find_library(EFI_LIB efi
        HINTS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
    )

    if(GNUEFI_INCLUDE_DIR AND GNUEFI_ARCH_INCLUDE_DIR
       AND GNUEFI_CRT0 AND GNUEFI_LDS AND GNUEFI_LIB AND EFI_LIB)

        set(EFI_NAME "canboot-${CANBOOT_ARCH}-uefi")
        set(EFI_SO  "${CMAKE_BINARY_DIR}/${EFI_NAME}.so")
        set(EFI_OUT "${CMAKE_BINARY_DIR}/${EFI_NAME}.efi")

        set(EFI_SOURCES
            boot/uefi/efi_main.c
            ${CANBOOT_KERNEL_COMMON}
            ${CANBOOT_LWIP_SOURCES}
            ${CANBOOT_CANDO_SOURCES}
            ${CANBOOT_NTFS3G_SOURCES}
        )

        set(EFI_OBJS "")
        set(_efi_obj_seq 0)
        foreach(src ${EFI_SOURCES})
            if(IS_ABSOLUTE ${src})
                set(_src_abs ${src})
            else()
                set(_src_abs ${CMAKE_SOURCE_DIR}/${src})
            endif()
            get_filename_component(_name ${src} NAME_WE)
            math(EXPR _efi_obj_seq "${_efi_obj_seq} + 1")
            set(_obj "${CMAKE_BINARY_DIR}/efi_${_efi_obj_seq}_${_name}.o")
            set(_extra_flags "")
            if(_src_abs MATCHES "/libntfs-3g/" OR _src_abs MATCHES "/ntfs3g_canboot_")
                set(_extra_flags
                    "-include" "${CMAKE_SOURCE_DIR}/cando_port/ntfs3g_canboot_shim.h"
                    -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot
                    -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include
                    -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g
                    -DHAVE_CONFIG_H=1
                    -w
                )
            elseif(_src_abs MATCHES "/ntfsprogs/")
                set(_extra_flags
                    "-include" "${CMAKE_SOURCE_DIR}/cando_port/ntfs3g_canboot_shim.h"
                    -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot
                    -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include
                    -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g
                    -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/ntfsprogs
                    -DCANBOOT_MKNTFS=1
                    -Dmain=mkntfs_main_canboot
                    -fcommon
                    -DHAVE_CONFIG_H=1
                    -w
                )
            endif()
            add_custom_command(
                OUTPUT ${_obj}
                COMMAND ${CMAKE_C_COMPILER}
                    ${_extra_flags}
                    -I${CMAKE_SOURCE_DIR}/cando_port/shims
                    -I${GNUEFI_INCLUDE_DIR}
                    -I${GNUEFI_ARCH_INCLUDE_DIR}
                    -I${CMAKE_SOURCE_DIR}/hal/include
                    -I${CMAKE_SOURCE_DIR}/kernel/include
                    -I${CMAKE_SOURCE_DIR}/rt/pthread_stub/include
                    -I${CMAKE_SOURCE_DIR}/net/lwip_port/include
                    -I${CMAKE_SOURCE_DIR}/net/mbedtls_port/include
                    -I${CMAKE_SOURCE_DIR}/net/mbedtls_port
                    -I${CMAKE_SOURCE_DIR}
                    -I${LWIP_DIR}/src/include
                    -I${MBEDTLS_DIR}/include
                    -I${CANDO_DIR}/include
                    -I${CANDO_DIR}/source
                    -I${PICOLIBC_INCLUDE}
                    -D_GNU_SOURCE
                    -DSSIZE_MAX=__LONG_MAX__
                    "-iquote" "${CANDO_DIR}/source"
                    "-iquote" "${CANDO_DIR}/source/core"
                    "-iquote" "${CANDO_DIR}/source/object"
                    "-iquote" "${CANDO_DIR}/source/parser"
                    "-iquote" "${CANDO_DIR}/source/vm"
                    "-iquote" "${CANDO_DIR}/source/jit"
                    "-iquote" "${CANDO_DIR}/source/lib"
                    -ffreestanding -fpic -fshort-wchar -mno-red-zone
                    -fno-stack-protector -fno-stack-check
                    -fcf-protection=none
                    -D_Thread_local= -D__thread=
                    -Wno-implicit-function-declaration
                    -Wno-int-conversion
                    -c ${_src_abs} -o ${_obj}
                DEPENDS ${_src_abs} picolibc_ext
                COMMENT "CC  efi/${_name}.o"
                VERBATIM
            )
            list(APPEND EFI_OBJS ${_obj})
        endforeach()

        # --allow-multiple-definition: gnu-efi's libefi ships its own
        # tiny memset/memcpy/memmove/strlen for EFI apps that link no
        # libc; picolibc supplies the same symbols. With picolibc listed
        # first in --start-group, its optimised x86 versions win and
        # the libefi copies are tolerated as harmless duplicates.
        # libgcc.a supplies the 128-bit divide builtins (__udivti3 etc.)
        # that picolibc's bignum / Mbed TLS's int math need.
        execute_process(
            COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
            OUTPUT_VARIABLE CANBOOT_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

        add_custom_command(
            OUTPUT ${EFI_SO}
            COMMAND ld -nostdlib -znocombreloc
                -T ${GNUEFI_LDS}
                -shared -Bsymbolic
                --allow-multiple-definition
                ${GNUEFI_CRT0} ${EFI_OBJS}
                -o ${EFI_SO}
                --start-group
                $<TARGET_FILE:mbedtls> $<TARGET_FILE:mbedx509> $<TARGET_FILE:mbedcrypto>
                ${PICOLIBC_LIB} ${EFI_LIB} ${GNUEFI_LIB} ${CANBOOT_LIBGCC}
                --end-group
            DEPENDS ${EFI_OBJS} ${GNUEFI_LDS} ${GNUEFI_CRT0} picolibc_ext
                    mbedtls mbedx509 mbedcrypto
            COMMENT "LD  ${EFI_NAME}.so"
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${EFI_OUT}
            COMMAND objcopy
                -j .text -j .sdata -j .data -j .dynamic
                -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.*
                -j .reloc
                --target efi-app-x86_64
                ${EFI_SO} ${EFI_OUT}
            DEPENDS ${EFI_SO}
            COMMENT "EFI ${EFI_NAME}.efi"
            VERBATIM
        )

        add_custom_target(canboot-uefi ALL DEPENDS ${EFI_OUT})
    else()
        message(WARNING
            "gnu-efi not found; UEFI target disabled. "
            "Install the `gnu-efi` package to enable it.")
    endif()
endif()

# ---------------------------------------------------------------------------
# aarch64 UEFI loader (milestone 14): cross-builds vendor/gnu-efi for
# aarch64, then links boot/uefi/efi_main_aarch64.c against it to produce
# canboot-aarch64-uefi.efi - a PE32+ EFI application that AAVMF loads,
# collects boot_info from, ExitBootServices, and hands off to the same
# stub aarch64 kmain the direct -kernel path uses.
#
# We vendor gnu-efi because the Ubuntu `gnu-efi` package only ships its
# host arch (x86_64); the aarch64 crt0/lds/libs would otherwise require
# arm64 multiarch which is brittle to bring up in CI.
# ---------------------------------------------------------------------------
if(CANBOOT_ARCH STREQUAL "aarch64")
    include(ExternalProject)

    set(GNUEFI_SRC     "${CMAKE_SOURCE_DIR}/vendor/gnu-efi")
    set(GNUEFI_PREFIX  "${CMAKE_BINARY_DIR}/gnu-efi")
    set(GNUEFI_BUILD   "${GNUEFI_PREFIX}/build")
    set(GNUEFI_INSTALL "${GNUEFI_PREFIX}/install")

    set(GNUEFI_AARCH64_CRT0  "${GNUEFI_INSTALL}/lib/crt0-efi-aarch64.o")
    set(GNUEFI_AARCH64_LDS   "${GNUEFI_INSTALL}/lib/elf_aarch64_efi.lds")
    set(GNUEFI_AARCH64_LIBE  "${GNUEFI_INSTALL}/lib/libefi.a")
    set(GNUEFI_AARCH64_LIBG  "${GNUEFI_INSTALL}/lib/libgnuefi.a")
    set(GNUEFI_INCLUDE       "${GNUEFI_INSTALL}/include/efi")
    set(GNUEFI_ARCH_INCLUDE  "${GNUEFI_INSTALL}/include/efi/aarch64")

    ExternalProject_Add(gnu_efi_ext
        SOURCE_DIR         ${GNUEFI_SRC}
        PREFIX             ${GNUEFI_PREFIX}
        BUILD_IN_SOURCE    FALSE
        CONFIGURE_COMMAND  ""
        BUILD_COMMAND      ${CMAKE_COMMAND} -E env
                               make -C ${GNUEFI_SRC}
                                   ARCH=aarch64
                                   CROSS_COMPILE=aarch64-linux-gnu-
                                   PREFIX=${GNUEFI_INSTALL}
        INSTALL_COMMAND    ${CMAKE_COMMAND} -E env
                               make -C ${GNUEFI_SRC}
                                   ARCH=aarch64
                                   CROSS_COMPILE=aarch64-linux-gnu-
                                   PREFIX=${GNUEFI_INSTALL}
                                   install
        BUILD_BYPRODUCTS   ${GNUEFI_AARCH64_CRT0}
                           ${GNUEFI_AARCH64_LDS}
                           ${GNUEFI_AARCH64_LIBE}
                           ${GNUEFI_AARCH64_LIBG}
        USES_TERMINAL_BUILD TRUE
    )

    set(EFI_NAME "canboot-${CANBOOT_ARCH}-uefi")
    set(EFI_SO   "${CMAKE_BINARY_DIR}/${EFI_NAME}.so")
    set(EFI_OUT  "${CMAKE_BINARY_DIR}/${EFI_NAME}.efi")

    # EFI app shares the kmain stub + serial driver with the direct-kernel
    # path. We recompile them here with -fpic so they're position-
    # independent inside the PE image. Each source becomes a separate
    # custom command producing a .o with EFI-flavoured flags.
    # On the UEFI build we link in real PCI + virtio + virtio-input
     # support (AAVMF enumerates PCIe and assigns BARs before
     # ExitBootServices). Direct -kernel still uses the input stub
     # since we don't do PCI bring-up there.
    # cando vendor sources, minus the x86_64-only JIT machine-code
    # emitter (cando_port/jit_codegen_stub_aarch64.c stubs it out).
    set(CANBOOT_CANDO_AARCH64_SOURCES "")
    foreach(_csrc ${CANBOOT_CANDO_SOURCES})
        if(NOT _csrc MATCHES "/jit/codegen\\.c$")
            list(APPEND CANBOOT_CANDO_AARCH64_SOURCES ${_csrc})
        endif()
    endforeach()

    set(EFI_AARCH64_SOURCES
        boot/uefi/efi_main_aarch64.c
        kernel/kmain_aarch64.c
        kernel/fb.c
        kernel/m5_selftest.c
        kernel/m6_nettest.c
        kernel/m7_tlstest.c
        kernel/m8_disktest.c
        kernel/m9_candotest.c
        kernel/canboot_test_ca.c
        hal/console/serial_aarch64.c
        hal/pci/pci_aarch64.c
        hal/virtio/virtio_pci.c
        hal/input/input_queue.c
        hal/input/virtio_input.c
        hal/net/virtio_net.c
        hal/disk/disk.c
        hal/disk/virtio_blk.c
        hal/display/display.c
        hal/display/virtio_gpu.c
        fs/iso9660.c
        fs/fat32.c
        net/lwip_port/sys_arch.c
        net/mbedtls_port/entropy.c
        net/mbedtls_port/inet_pton.c
        net/mbedtls_port/lwip_bio.c
        rt/picolibc_port/syscalls.c
        rt/pthread_stub/pthread.c
        cando_port/cando_stubs.c
        cando_port/cando_display_lib.c
        cando_port/cando_input_lib.c
        cando_port/cando_time_lib.c
        cando_port/cando_file_lib.c
        cando_port/cando_net_lib.c
        cando_port/cando_tls_lib.c
        cando_port/cando_random_lib.c
        cando_port/cando_crypto_lib.c
        cando_port/cando_encoding_lib.c
        cando_port/cando_log_lib.c
        cando_port/cando_env_lib.c
        cando_port/cando_url_lib.c
        cando_port/cando_http_lib.c
        cando_port/cando_disk_lib.c
        cando_port/cando_pci_lib.c
        cando_port/cando_fb_lib.c
        cando_port/cando_fmt_lib.c
        cando_port/cando_partition_lib.c
        cando_port/cando_fs_lib.c
        cando_port/ntfs3g_canboot_io.c
        cando_port/ntfs3g_canboot_glue.c
        cando_port/jit_codegen_stub_aarch64.c
        fs/partition.c
        fs/ntfs.c
        kernel/env.c
        ${CANBOOT_NTFS3G_SOURCES}
        ${CANBOOT_LWIP_SOURCES}
        ${CANBOOT_CANDO_AARCH64_SOURCES}
    )

    set(EFI_AARCH64_OBJS "")
    set(_efi_obj_seq 0)
    foreach(src ${EFI_AARCH64_SOURCES})
        if(IS_ABSOLUTE "${src}")
            set(_src_abs "${src}")
        else()
            set(_src_abs ${CMAKE_SOURCE_DIR}/${src})
        endif()
        get_filename_component(_name ${src} NAME_WE)
        math(EXPR _efi_obj_seq "${_efi_obj_seq} + 1")
        set(_obj "${CMAKE_BINARY_DIR}/efi_aarch64_${_efi_obj_seq}_${_name}.o")
        set(_extra_flags "")
        if(_src_abs MATCHES "/libntfs-3g/" OR _src_abs MATCHES "/ntfs3g_canboot_")
            set(_extra_flags
                "-include" "${CMAKE_SOURCE_DIR}/cando_port/ntfs3g_canboot_shim.h"
                -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot
                -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include
                -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g
                -DHAVE_CONFIG_H=1
                -w
            )
        endif()
        add_custom_command(
            OUTPUT ${_obj}
            COMMAND aarch64-linux-gnu-gcc
                ${_extra_flags}
                -I${CMAKE_SOURCE_DIR}/cando_port/shims
                -I${GNUEFI_INCLUDE}
                -I${GNUEFI_ARCH_INCLUDE}
                -I${CMAKE_SOURCE_DIR}/hal/include
                -I${CMAKE_SOURCE_DIR}/kernel/include
                -I${CMAKE_SOURCE_DIR}/rt/pthread_stub/include
                -I${CMAKE_SOURCE_DIR}/net/lwip_port/include
                -I${CMAKE_SOURCE_DIR}/net/mbedtls_port/include
                -I${CMAKE_SOURCE_DIR}/net/mbedtls_port
                -I${CMAKE_SOURCE_DIR}
                -I${LWIP_DIR}/src/include
                -I${MBEDTLS_DIR}/include
                -I${CANDO_DIR}/include
                -I${CANDO_DIR}/source
                -I${PICOLIBC_INCLUDE}
                "-iquote" "${CANDO_DIR}/source"
                "-iquote" "${CANDO_DIR}/source/core"
                "-iquote" "${CANDO_DIR}/source/object"
                "-iquote" "${CANDO_DIR}/source/parser"
                "-iquote" "${CANDO_DIR}/source/vm"
                "-iquote" "${CANDO_DIR}/source/jit"
                "-iquote" "${CANDO_DIR}/source/lib"
                -DCANBOOT_AARCH64_EFI_BUILD=1
                -D_GNU_SOURCE
                -DSSIZE_MAX=__LONG_MAX__
                -D_Thread_local= -D__thread=
                -ffreestanding -fpic -fno-stack-protector -fno-stack-check
                -fno-strict-aliasing -fshort-wchar
                -Wno-implicit-function-declaration
                -Wno-int-conversion
                -Wall -Wextra
                -c ${_src_abs} -o ${_obj}
            DEPENDS ${_src_abs} gnu_efi_ext picolibc_ext
            COMMENT "CC  efi-aarch64/${_name}.o"
            VERBATIM
        )
        list(APPEND EFI_AARCH64_OBJS ${_obj})
    endforeach()

    # Resolve aarch64 libgcc.a path (provides __divti3 etc. that picolibc
     # may reference but doesn't ship). gcc -print-libgcc-file-name gives
     # us the right copy for the cross toolchain.
    execute_process(
        COMMAND aarch64-linux-gnu-gcc -print-libgcc-file-name
        OUTPUT_VARIABLE CANBOOT_AARCH64_LIBGCC
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    add_custom_command(
        OUTPUT ${EFI_SO}
        COMMAND aarch64-linux-gnu-ld -nostdlib -znocombreloc
            -T ${GNUEFI_AARCH64_LDS}
            -shared -Bsymbolic
            --allow-multiple-definition
            ${GNUEFI_AARCH64_CRT0} ${EFI_AARCH64_OBJS}
            -o ${EFI_SO}
            -L${GNUEFI_INSTALL}/lib
            --start-group
                $<TARGET_FILE:mbedtls> $<TARGET_FILE:mbedx509> $<TARGET_FILE:mbedcrypto>
                -lefi -lgnuefi ${PICOLIBC_LIB} ${CANBOOT_AARCH64_LIBGCC}
            --end-group
        DEPENDS ${EFI_AARCH64_OBJS} gnu_efi_ext picolibc_ext
                mbedtls mbedx509 mbedcrypto
        COMMENT "LD  ${EFI_NAME}.so"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${EFI_OUT}
        COMMAND aarch64-linux-gnu-objcopy
            -j .text -j .sdata -j .data -j .dynamic
            -j .rodata
            -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.*
            -j .areloc -j .reloc
            --target efi-app-aarch64
            ${EFI_SO} ${EFI_OUT}
        DEPENDS ${EFI_SO}
        COMMENT "EFI ${EFI_NAME}.efi"
        VERBATIM
    )

    add_custom_target(canboot-uefi ALL DEPENDS ${EFI_OUT})
endif()
