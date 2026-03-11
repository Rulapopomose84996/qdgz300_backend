# cmake/CompilerOptions.cmake — QDGZ300 统一编译选项
#
# 目标: ARM64 (aarch64) / Clang 18 / C++17 / -O2
# 包含: 通用警告、ARM64 优化、覆盖率支持

# ═══ 通用警告 ═══
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter # 接口变量可能暂未使用
    -Werror=return-type # 缺少返回值视为错误
)

# ═══ ARM64 优化 ═══
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    add_compile_options(
        -march=armv8-a+crypto # CRC32C 硬件加速
        -mtune=cortex-a72 # 飞腾 S5000C 通用调优
    )

    # Release 模式使用 -O3
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
else()
    # x86_64 主机编译
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif()

# ═══ Debug / RelWithDebInfo ═══
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DQDGZ300_DEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "" FORCE)

message(STATUS "CompilerOptions.cmake loaded for ${CMAKE_SYSTEM_PROCESSOR}")
