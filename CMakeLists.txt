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
