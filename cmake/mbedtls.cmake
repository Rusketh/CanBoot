# cmake/mbedtls.cmake
#
# Pulls in Mbed TLS 3.6.x (vendored at vendor/mbedtls) via
# add_subdirectory(EXCLUDE_FROM_ALL). Our freestanding cross flags are
# pre-applied via add_compile_options() so they reach mbedtls's targets
# (mbedtls / mbedx509 / mbedcrypto) automatically. MBEDTLS_FATAL_WARNINGS
# is off because freestanding flags trip a few warnings that we have
# no business fixing upstream.
#
# Inputs: CANBOOT_ARCH, PICOLIBC_INCLUDE (must be set first).

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

# -D_Thread_local= flattens cando's four _Thread_local statics into
# plain globals (mbedtls doesn't use TLS itself in our config but we
# keep the same flags so the EFI link sees identical TUs).
if(CANBOOT_ARCH STREQUAL "x86_64")
    add_compile_options(
        -ffreestanding -fno-stack-protector -fno-stack-check
        -mno-red-zone -fPIC -fcf-protection=none
        -D_Thread_local= -D__thread=
    )
else()
    # On aarch64-cross gcc, /usr/include/limits.h pulls in
    # bits/libc-header-start.h which only exists if
    # libc6-dev-arm64-cross is installed. picolibc has the same headers
    # in a self-contained form; we route mbedtls through them by
    # prepending -I (BEFORE without SYSTEM emits a plain -I, which gcc
    # searches *before* its internal include-fixed tree - the only way
    # to win against /usr/include/limits.h).
    include_directories(BEFORE ${PICOLIBC_INCLUDE})
    add_compile_options(
        -ffreestanding -fno-stack-protector -fno-stack-check
        -fPIC
        -D_Thread_local= -D__thread=
    )
endif()
add_subdirectory(${MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)

# On aarch64 mbedtls includes picolibc's <limits.h>. Force the mbedtls
# targets to wait for picolibc_ext so the install/include tree exists
# before any mbedtls source compiles.
if(CANBOOT_ARCH STREQUAL "aarch64")
    foreach(_t mbedtls mbedx509 mbedcrypto everest p256m)
        if(TARGET ${_t})
            add_dependencies(${_t} picolibc_ext)
        endif()
    endforeach()
endif()
