# cmake/uefi_x86_64.cmake
#
# Builds the x86_64 UEFI loader (canboot-x86_64-uefi.efi). Compiles the
# shared kernel sources with PIC + short-wchar + EFI include path, links
# against gnu-efi's crt0 and elf_x86_64_efi.lds, then objcopies to a
# PE32+ EFI application. Calls into the same kmain as the BIOS kernel.
#
# Inputs from parent scope: CANBOOT_KERNEL_COMMON, CANBOOT_LWIP_SOURCES,
# CANBOOT_CANDO_SOURCES, CANBOOT_NTFS3G_SOURCES,
# CANBOOT_NTFS3G_MKFS_SOURCES, CANBOOT_LWEXT4_SOURCES, LWIP_DIR,
# MBEDTLS_DIR, CANDO_DIR, PICOLIBC_INCLUDE, PICOLIBC_LIB.
#
# Adds target canboot-uefi (ALL).

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
        ${CANBOOT_NTFS3G_MKFS_SOURCES}
        ${CANBOOT_LWEXT4_SOURCES}
        hal/audio/intel_hda.c
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
                -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g_canboot
                -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include
                -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/include/ntfs-3g
                -I${CMAKE_SOURCE_DIR}/vendor/ntfs-3g/ntfsprogs
                -DCANBOOT_MKNTFS=1
                "-include" "${CMAKE_SOURCE_DIR}/cando_port/vendor_glue/ntfs3g/mkntfs_main_rename.h"
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
    # tiny memset/memcpy/memmove/strlen for EFI apps that link no libc;
    # picolibc supplies the same symbols. With picolibc listed first
    # in --start-group, its optimised x86 versions win and the libefi
    # copies are tolerated as harmless duplicates. libgcc.a supplies
    # the 128-bit divide builtins (__udivti3 etc.) that picolibc's
    # bignum / Mbed TLS's int math need.
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
