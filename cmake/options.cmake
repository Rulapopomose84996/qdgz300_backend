# Project-wide options and baseline compiler settings.

option(QDGZ300_ENABLE_COVERAGE "Enable code coverage (GCC/Clang only)" OFF)
option(QDGZ300_BUILD_SIMULATOR "Build simulator executable" OFF)
option(QDGZ300_ENABLE_GPU "Enable GPU (Iluvatar CoreX) support" OFF)
option(QDGZ300_ENABLE_PROTOBUF "Enable Protobuf for M04 Gateway" OFF)

include(CTest)
set(BUILD_TESTING "${BUILD_TESTING}" CACHE BOOL "CTest build flag" FORCE)
set(QDGZ300_BUILD_TESTING "${BUILD_TESTING}" CACHE BOOL "Build tests" FORCE)
set(BUILD_SIMULATOR "${QDGZ300_BUILD_SIMULATOR}" CACHE BOOL "Backward-compatible simulator flag" FORCE)
set(ENABLE_GPU "${QDGZ300_ENABLE_GPU}" CACHE BOOL "Backward-compatible GPU flag" FORCE)
set(ENABLE_PROTOBUF "${QDGZ300_ENABLE_PROTOBUF}" CACHE BOOL "Backward-compatible protobuf flag" FORCE)

if(CMAKE_CROSSCOMPILING AND EXISTS "${CMAKE_SOURCE_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake")
    message(STATUS "Cross-compiling for aarch64 with existing toolchain file")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "This project is designed for Linux. Current system: ${CMAKE_SYSTEM_NAME}")
endif()

set(DEPS_OFFLINE_ROOT "${CMAKE_SOURCE_DIR}/deps_offline/extracted" CACHE PATH "Extracted offline dependency root")
