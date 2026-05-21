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

set(KERNEL_NAME "canboot-${CANBOOT_ARCH}")
set(KERNEL_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/linker/kernel.${CANBOOT_ARCH}.ld")

if(CANBOOT_ARCH STREQUAL "x86_64")
    set(KERNEL_SOURCES
        arch/x86_64/bootstrap.S
        arch/x86_64/kmain.c
        boot/multiboot2/header.S
        hal/console/serial_x86.c
    )
else()
    message(FATAL_ERROR "Arch ${CANBOOT_ARCH} bring-up lands in a later milestone")
endif()

add_executable(${KERNEL_NAME} ${KERNEL_SOURCES})

set_source_files_properties(
    arch/x86_64/bootstrap.S
    boot/multiboot2/header.S
    PROPERTIES LANGUAGE ASM
)

target_include_directories(${KERNEL_NAME} PRIVATE
    hal/include
)

target_compile_options(${KERNEL_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
    $<$<COMPILE_LANGUAGE:C>:-fno-stack-protector>
    $<$<COMPILE_LANGUAGE:C>:-fno-stack-check>
    $<$<COMPILE_LANGUAGE:C>:-fno-pic>
    $<$<COMPILE_LANGUAGE:C>:-fno-pie>
    $<$<COMPILE_LANGUAGE:C>:-mno-red-zone>
    $<$<COMPILE_LANGUAGE:C>:-mcmodel=kernel>
    $<$<COMPILE_LANGUAGE:C>:-mno-sse>
    $<$<COMPILE_LANGUAGE:C>:-mno-mmx>
    $<$<COMPILE_LANGUAGE:C>:-mno-80387>
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

set_target_properties(${KERNEL_NAME} PROPERTIES
    SUFFIX ".elf"
    LINK_DEPENDS ${KERNEL_LINKER_SCRIPT}
)

# Convenience target: print artifact location
add_custom_target(canboot-info ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Kernel ELF: $<TARGET_FILE:${KERNEL_NAME}>"
    DEPENDS ${KERNEL_NAME}
)

# ---------------------------------------------------------------------------
# UEFI loader (gnu-efi). Built as a separate PE/COFF binary; in milestone 2
# it stands alone, in milestone 3 it will load the kernel ELF and hand off
# to kmain() with a normalised boot_info.
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
        set(EFI_SRC "${CMAKE_SOURCE_DIR}/boot/uefi/efi_main.c")
        set(EFI_OBJ "${CMAKE_BINARY_DIR}/${EFI_NAME}.o")
        set(EFI_SO  "${CMAKE_BINARY_DIR}/${EFI_NAME}.so")
        set(EFI_OUT "${CMAKE_BINARY_DIR}/${EFI_NAME}.efi")

        add_custom_command(
            OUTPUT ${EFI_OBJ}
            COMMAND ${CMAKE_C_COMPILER}
                -I${GNUEFI_INCLUDE_DIR}
                -I${GNUEFI_ARCH_INCLUDE_DIR}
                -ffreestanding -fpic -fshort-wchar -mno-red-zone
                -fno-stack-protector -fno-stack-check
                -Wall -Wextra
                -c ${EFI_SRC} -o ${EFI_OBJ}
            DEPENDS ${EFI_SRC}
            COMMENT "CC  ${EFI_NAME}.o"
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${EFI_SO}
            COMMAND ld -nostdlib -znocombreloc
                -T ${GNUEFI_LDS}
                -shared -Bsymbolic
                ${GNUEFI_CRT0} ${EFI_OBJ}
                -o ${EFI_SO}
                ${EFI_LIB} ${GNUEFI_LIB}
            DEPENDS ${EFI_OBJ} ${GNUEFI_LDS} ${GNUEFI_CRT0}
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
