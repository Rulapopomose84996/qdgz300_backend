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

if(WIN32)
    set(_qdgz300_default_cache_root "D:/WorkSpace/ThirdPartyCache/${PROJECT_NAME}")
    set(_qdgz300_default_deps_root "${_qdgz300_default_cache_root}/build/windows-x64")
elseif(CMAKE_CROSSCOMPILING)
    set(_qdgz300_default_cache_root "/mnt/d/WorkSpace/ThirdPartyCache/${PROJECT_NAME}")
    set(_qdgz300_default_deps_root "${_qdgz300_default_cache_root}/build/wsl-aarch64")
else()
    set(_qdgz300_default_cache_root "/home/devuser/WorkSpace/ThirdPartyCache/${PROJECT_NAME}")
    set(_qdgz300_default_deps_root "${_qdgz300_default_cache_root}/build/native-aarch64")
endif()

set(QDGZ300_THIRD_PARTY_CACHE_ROOT "${_qdgz300_default_cache_root}" CACHE PATH
    "Shared third-party cache root")
set(QDGZ300_OFFLINE_DEPS_DIR "${QDGZ300_THIRD_PARTY_CACHE_ROOT}/archives" CACHE PATH
    "Shared offline dependency archive directory")
set(QDGZ300_DEPS_ROOT "${_qdgz300_default_deps_root}" CACHE PATH
    "Shared dependency cache workspace")
set(QDGZ300_DEPS_PREFIX "${QDGZ300_DEPS_ROOT}/prefix" CACHE PATH
    "Shared dependency prefix directory")
set(DEPS_OFFLINE_ROOT "" CACHE PATH
    "Legacy extracted offline dependency root. Leave empty to disable project-local fallback")
