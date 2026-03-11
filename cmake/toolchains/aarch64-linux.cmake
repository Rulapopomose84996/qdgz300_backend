# Tower receiver_app - CMake toolchain for ARM64 (aarch64) cross-compilation on Linux

# Target platform
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross compilers
find_program(AARCH64_GCC aarch64-linux-gnu-gcc)
find_program(AARCH64_GXX aarch64-linux-gnu-g++)

if(NOT AARCH64_GCC OR NOT AARCH64_GXX)
    message(FATAL_ERROR "aarch64-linux-gnu toolchain not found. Install it first (e.g. run tools/环境/Linux交叉编译环境初始化.sh).")
endif()

set(CMAKE_C_COMPILER ${AARCH64_GCC})
set(CMAKE_CXX_COMPILER ${AARCH64_GXX})

# Optional sysroot (created by tools/setup_wsl_cross_compile.sh)
set(_AARCH64_SYSROOT "$ENV{AARCH64_SYSROOT}")

if(_AARCH64_SYSROOT AND IS_DIRECTORY "${_AARCH64_SYSROOT}")
    set(CMAKE_SYSROOT "${_AARCH64_SYSROOT}")

    # Make CMake find headers/libs inside sysroot
    list(APPEND CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
endif()

# Default ARM64 optimization flags for cross builds.
set(CMAKE_C_FLAGS_INIT "-march=armv8-a+crypto")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a+crypto")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O3")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3")

# Search behavior for programs/libs/headers when cross-compiling
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Use QEMU for running ARM64 binaries on x86_64 (for testing)
find_program(QEMU_AARCH64 qemu-aarch64-static)
if(QEMU_AARCH64)
    set(_AARCH64_QEMU_ROOT "")

    # Prefer explicit CMAKE_SYSROOT, then env var, then common Ubuntu multiarch location.
    if(CMAKE_SYSROOT AND EXISTS "${CMAKE_SYSROOT}/lib/ld-linux-aarch64.so.1")
        set(_AARCH64_QEMU_ROOT "${CMAKE_SYSROOT}")
    elseif(_AARCH64_SYSROOT AND EXISTS "${_AARCH64_SYSROOT}/lib/ld-linux-aarch64.so.1")
        set(_AARCH64_QEMU_ROOT "${_AARCH64_SYSROOT}")
    elseif(EXISTS "/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1")
        set(_AARCH64_QEMU_ROOT "/usr/aarch64-linux-gnu")
    endif()

    if(_AARCH64_QEMU_ROOT)
        set(CMAKE_CROSSCOMPILING_EMULATOR "${QEMU_AARCH64};-L;${_AARCH64_QEMU_ROOT}")
    else()
        set(CMAKE_CROSSCOMPILING_EMULATOR "${QEMU_AARCH64}")
        message(WARNING "ARM64 loader not found for qemu. Set AARCH64_SYSROOT to run cross-compiled tests.")
    endif()

    message(STATUS "Cross-compilation emulator: ${CMAKE_CROSSCOMPILING_EMULATOR}")
endif()
