# Project-wide options and baseline compiler settings.

option(ENABLE_COVERAGE "Enable code coverage (GCC/Clang only)" OFF)
option(BUILD_SIMULATOR "Build simulator executable" OFF)
option(ENABLE_GPU "Enable GPU (Iluvatar CoreX) support" OFF)
option(ENABLE_PROTOBUF "Enable Protobuf for M04 Gateway" OFF)

include(CTest)

if(CMAKE_CROSSCOMPILING AND EXISTS "${CMAKE_SOURCE_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake")
    message(STATUS "Cross-compiling for aarch64 with existing toolchain file")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "This project is designed for Linux. Current system: ${CMAKE_SYSTEM_NAME}")
endif()

set(DEPS_OFFLINE_ROOT "${CMAKE_SOURCE_DIR}/deps_offline/extracted" CACHE PATH "Extracted offline dependency root")
