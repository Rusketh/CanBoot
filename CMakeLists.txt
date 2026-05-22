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
    kernel/kmain.c
    kernel/fb.c
    kernel/m5_selftest.c
    kernel/m6_nettest.c
    kernel/m7_tlstest.c
    kernel/m8_disktest.c
    kernel/m9_candotest.c
    kernel/canboot_test_ca.c
    cando_port/cando_stubs.c
    fs/iso9660.c
    fs/fat32.c
    hal/disk/disk.c
    hal/disk/virtio_blk.c
    hal/disk/ahci.c
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
    ${CANDO_DIR}/source/lib/file.c
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
    ${CANDO_DIR}/source/lib/net.c
    ${CANDO_DIR}/source/lib/object.c
    ${CANDO_DIR}/source/lib/meta.c
    ${CANDO_DIR}/source/lib/stream.c
)

# ---------------------------------------------------------------------------
# picolibc: vendored at vendor/picolibc, built via meson at our build time.
# Provides full libc (stdio, stdlib, string, setjmp, errno) for the kernel +
# UEFI. posix-console=true routes stdin/stdout/stderr through our syscall
# stubs in rt/picolibc_port/.
# ---------------------------------------------------------------------------
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
    )
else()
    message(FATAL_ERROR "Arch ${CANBOOT_ARCH} bring-up lands in a later milestone")
endif()

add_executable(${KERNEL_NAME} ${KERNEL_SOURCES})
add_dependencies(${KERNEL_NAME} picolibc_ext)

set_source_files_properties(
    arch/x86_64/bootstrap.S
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
    -Wall
    -Wextra
)

target_compile_options(${KERNEL_NAME} PRIVATE
    -D_GNU_SOURCE     # cando expects M_PI, strdup, etc. via GNU extensions
    -DSSIZE_MAX=__LONG_MAX__   # signal lwIP arch.h to use picolibc's ssize_t
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

# Link picolibc as the kernel's libc. Put it inside a --start-group/end-group
# pair so circular references between picolibc and our syscall stubs (e.g.
# malloc -> sbrk; printf -> write) resolve in one pass.
target_link_libraries(${KERNEL_NAME} PRIVATE
    -Wl,--start-group
    mbedtls mbedx509 mbedcrypto
    ${PICOLIBC_LIB}
    -Wl,--end-group
    -lgcc
)

set_target_properties(${KERNEL_NAME} PROPERTIES
    SUFFIX ".elf"
    LINK_DEPENDS ${KERNEL_LINKER_SCRIPT}
)

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
if(CANBOOT_ARCH STREQUAL "x86_64")
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

    add_compile_options(
        -ffreestanding -fno-stack-protector -fno-stack-check
        -mno-red-zone -fPIC
    )
    add_subdirectory(${MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)
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
            add_custom_command(
                OUTPUT ${_obj}
                COMMAND ${CMAKE_C_COMPILER}
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
                ${PICOLIBC_LIB} ${EFI_LIB} ${GNUEFI_LIB}
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
