# Cross-toolchain file for aarch64 bare-metal builds. Pass via:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
#         -DCANBOOT_ARCH=aarch64

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER  aarch64-linux-gnu-gcc)
set(CMAKE_ASM_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_AR          aarch64-linux-gnu-ar)
set(CMAKE_LINKER      aarch64-linux-gnu-ld)
set(CMAKE_OBJCOPY     aarch64-linux-gnu-objcopy)
set(CMAKE_STRIP       aarch64-linux-gnu-strip)

set(CMAKE_C_COMPILER_WORKS    1)
set(CMAKE_ASM_COMPILER_WORKS  1)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
