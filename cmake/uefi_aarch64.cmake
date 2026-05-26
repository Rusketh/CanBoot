# cmake/uefi_aarch64.cmake
#
# Cross-builds vendor/gnu-efi for aarch64, then links
# boot/uefi/efi_main_aarch64.c against it to produce
# canboot-aarch64-uefi.efi - a PE32+ EFI application that AAVMF loads,
# collects boot_info from, ExitBootServices, and hands off to the same
# stub aarch64 kmain the direct -kernel path uses.
#
# We vendor gnu-efi because the Ubuntu `gnu-efi` package only ships its
# host arch (x86_64); the aarch64 crt0/lds/libs would otherwise require
# arm64 multiarch which is brittle to bring up in CI.
#
# Inputs from parent scope: CANBOOT_CANDO_SOURCES, CANBOOT_NTFS3G_SOURCES,
# CANBOOT_NTFS3G_MKFS_SOURCES, CANBOOT_LWEXT4_SOURCES,
# CANBOOT_LWIP_SOURCES, LWIP_DIR, MBEDTLS_DIR, CANDO_DIR,
# PICOLIBC_INCLUDE, PICOLIBC_LIB.
#
# Adds target canboot-uefi (ALL).

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
# custom command producing a .o with EFI-flavoured flags. On the UEFI
# build we link in real PCI + virtio + virtio-input support (AAVMF
# enumerates PCIe and assigns BARs before ExitBootServices). Direct
# -kernel still uses the input stub since we don't do PCI bring-up
# there.
# cando vendor sources, minus the x86_64-only JIT machine-code emitter
# (cando_port/jit/codegen_stub_aarch64.c stubs it out).
set(CANBOOT_CANDO_AARCH64_SOURCES "")
foreach(_csrc ${CANBOOT_CANDO_SOURCES})
    if(NOT _csrc MATCHES "/jit/codegen\\.c$")
        list(APPEND CANBOOT_CANDO_AARCH64_SOURCES ${_csrc})
    endif()
endforeach()

set(EFI_AARCH64_SOURCES
    # aarch64-specific kernel + EFI entry.
    boot/uefi/efi_main_aarch64.c
    kernel/kmain_aarch64.c

    # aarch64-specific HAL: PL011 console, PCIe ECAM, virtio-gpu (no
    # firmware framebuffer on virt; we drive the scanout ourselves),
    # virtio-sound (Intel HDA isn't available on the aarch64 virt
    # machine), JIT codegen stub (the x86_64 emitter doesn't compile
    # under aarch64).
    hal/console/serial_aarch64.c
    hal/pci/pci_aarch64.c
    hal/display/virtio_gpu.c
    hal/audio/virtio_snd.c
    cando_port/jit/codegen_stub_aarch64.c

    # aarch64 EL1 vector table + GICv2/generic-timer IRQ path (M4
    # preemption), shared with the direct-kernel build.
    arch/aarch64/vectors.S
    arch/aarch64/irq.c

    # aarch64 thread context switch (the scheduler core itself,
    # rt/sched/sched.c, rides in via CANBOOT_PORTABLE_SOURCES below).
    rt/sched/arch/ctx_aarch64.S

    # Everything else is the same set both arches build. Sourced
    # from cmake/sources.cmake so adding a new cando_port/lib/*.c
    # reaches the aarch64-UEFI build without per-arch source-list
    # maintenance (the silent-drift class of bug that produced the
    # b09db43 missing-symbol regression).
    ${CANBOOT_PORTABLE_SOURCES}

    ${CANBOOT_NTFS3G_SOURCES}
    ${CANBOOT_NTFS3G_MKFS_SOURCES}
    ${CANBOOT_LWEXT4_SOURCES}
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
    if(_src_abs MATCHES "/libntfs-3g/" OR _src_abs MATCHES "/vendor_glue/ntfs3g/")
        set(_extra_flags
            "-include" "${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/shim.h"
            -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot
            -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include
            -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g
            -DHAVE_CONFIG_H=1
            -w
        )
    elseif(_src_abs MATCHES "/ntfsprogs/")
        set(_extra_flags
            "-include" "${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/shim.h"
            "-include" "${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/mkntfs_main_rename.h"
            -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot
            -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include
            -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g
            -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/ntfsprogs
            -DCANBOOT_MKNTFS=1
            -fno-plt
            -fvisibility=hidden
            -fcommon
            -DHAVE_CONFIG_H=1
            -w
        )
    elseif(_src_abs MATCHES "/lwext4/" OR _src_abs MATCHES "/vendor_glue/lwext4/")
        set(_extra_flags
            -I${CMAKE_SOURCE_DIR}/vendor/lwext4/include
            -I${CMAKE_SOURCE_DIR}/vendor/lwext4_canboot
            -I${CMAKE_SOURCE_DIR}/cando_port
            -DCONFIG_USE_DEFAULT_CFG=1
            -DCONFIG_DEBUG_PRINTF=0
            -w
        )
    elseif(_src_abs MATCHES "/vendor_glue/stb/image")
        set(_extra_flags
            -I${CMAKE_SOURCE_DIR}/vendor/stb
            -w
        )
    elseif(_src_abs MATCHES "/vendor_glue/minimp3/decoder")
        set(_extra_flags
            -I${CMAKE_SOURCE_DIR}/vendor/minimp3
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
            -I${CMAKE_SOURCE_DIR}/rt/sched/include
            -I${CMAKE_SOURCE_DIR}/rt/sync/include
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
            # See CMakeLists.txt: keep libgcc's outline-atomics / LSE
            # runtime-detect (__getauxval) out of the freestanding link.
            -mno-outline-atomics
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
# may reference but doesn't ship). gcc -print-libgcc-file-name gives us
# the right copy for the cross toolchain.
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
        # M5: serialise the picolibc allocator (see syscalls.c __wrap_*).
        --wrap=malloc --wrap=free --wrap=calloc --wrap=realloc
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
