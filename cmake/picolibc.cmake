# cmake/picolibc.cmake
#
# Cross-builds picolibc for ${CANBOOT_ARCH} via ExternalProject. Used
# by both kernel and EFI builds.
#
# Exports: PICOLIBC_INCLUDE, PICOLIBC_LIB, target picolibc_ext.

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
