#!/usr/bin/env bash
# Configure, build, and install picolibc into our build tree. Called by
# CMake's ExternalProject_Add. Handles the meson "build dir already
# exists" case via --reconfigure so iterative CMake re-runs work.

set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <picolibc-src> <picolibc-build> <picolibc-prefix> <cross-file>" >&2
    exit 2
fi

SRC="$1"
BUILD="$2"
PREFIX="$3"
CROSS="$4"

OPTS=(
    --cross-file "$CROSS"
    --prefix "$PREFIX"
    --buildtype release
    -Dmultilib=false
    -Datomic-ungetc=false
    -Dformat-default=integer
    -Dpicocrt=false
    -Dpicocrt-lib=false
    -Dassert-verbose=false
    -Dthread-local-storage=false
    -Dsingle-thread=true
    -Dtests=false
    -Dfortify-source=none
    -Dfreestanding=true
    -Dsemihost=false
    -Dfake-semihost=false
    -Dposix-console=true
    -Dnewlib-global-errno=true
    -Dspecsdir=none
)

if [ -f "$BUILD/build.ninja" ]; then
    meson setup --reconfigure "$BUILD" "$SRC" "${OPTS[@]}" >/dev/null
else
    mkdir -p "$BUILD"
    meson setup "$BUILD" "$SRC" "${OPTS[@]}" >/dev/null
fi

ninja -C "$BUILD" >/dev/null
ninja -C "$BUILD" install >/dev/null
echo "picolibc: built and installed to $PREFIX"
