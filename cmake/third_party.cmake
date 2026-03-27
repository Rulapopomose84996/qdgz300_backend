# Third-party dependency setup.
# Prefer shared prefix imports first, then fall back to explicitly provided
# extracted offline sources. Online fetching is intentionally not supported.

function(qdgz300_create_spdlog_import prefix_dir)
    if(TARGET spdlog::spdlog)
        return()
    endif()

    set(_include_dir "${prefix_dir}/include")
    if(NOT EXISTS "${_include_dir}/spdlog/spdlog.h")
        message(FATAL_ERROR "spdlog headers not found under shared prefix: ${_include_dir}")
    endif()

    add_library(spdlog::spdlog INTERFACE IMPORTED GLOBAL)
    set_target_properties(spdlog::spdlog PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
    )
endfunction()

function(qdgz300_find_prefix_library out_var prefix_dir)
    set(_library_names ${ARGN})
    set(_library_path "")
    set(_arch_dir "${CMAKE_LIBRARY_ARCHITECTURE}")

    set(_search_dirs
        "${prefix_dir}/lib"
        "${prefix_dir}/lib64"
        "${prefix_dir}/Lib"
        "${prefix_dir}/bin"
    )

    if(_arch_dir)
        list(APPEND _search_dirs
            "${prefix_dir}/lib/${_arch_dir}"
            "${prefix_dir}/Lib/${_arch_dir}"
        )
    endif()

    list(APPEND _search_dirs
        "${prefix_dir}/lib/aarch64-linux-gnu"
        "${prefix_dir}/lib/x86_64-linux-gnu"
    )

    foreach(_dir IN LISTS _search_dirs)
        foreach(_name IN LISTS _library_names)
            if(EXISTS "${_dir}/${_name}")
                set(_library_path "${_dir}/${_name}")
                break()
            endif()
        endforeach()
        if(_library_path)
            break()
        endif()
    endforeach()

    set(${out_var} "${_library_path}" PARENT_SCOPE)
endfunction()

function(qdgz300_create_static_import imported_target prefix_dir)
    if(TARGET "${imported_target}")
        return()
    endif()

    set(_include_dir "${prefix_dir}/include")
    if(NOT EXISTS "${_include_dir}")
        message(FATAL_ERROR "Shared prefix include directory not found: ${_include_dir}")
    endif()

    qdgz300_find_prefix_library(_library_path "${prefix_dir}" ${ARGN})
    if(NOT _library_path)
        list(JOIN ARGN ", " _expected_names)
        message(FATAL_ERROR
            "Static library for ${imported_target} not found under shared prefix ${prefix_dir}. "
            "Expected one of: ${_expected_names}")
    endif()

    add_library("${imported_target}" STATIC IMPORTED GLOBAL)
    set_target_properties("${imported_target}" PROPERTIES
        IMPORTED_LOCATION "${_library_path}"
        INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
    )
endfunction()

function(qdgz300_try_import_from_shared_prefix)
    if(NOT QDGZ300_DEPS_PREFIX)
        return()
    endif()

    if(NOT IS_DIRECTORY "${QDGZ300_DEPS_PREFIX}")
        return()
    endif()

    message(STATUS "Using shared dependency prefix: ${QDGZ300_DEPS_PREFIX}")

    qdgz300_create_spdlog_import("${QDGZ300_DEPS_PREFIX}")
    qdgz300_create_static_import(
        yaml-cpp::yaml-cpp
        "${QDGZ300_DEPS_PREFIX}"
        yaml-cpp.lib
        yaml-cppd.lib
        libyaml-cpp.a
        libyaml-cppd.a
    )

    if(QDGZ300_BUILD_TESTING)
        qdgz300_create_static_import(
            GTest::gtest
            "${QDGZ300_DEPS_PREFIX}"
            gtest.lib
            gtestd.lib
            libgtest.a
            libgtestd.a
        )
        qdgz300_create_static_import(
            GTest::gtest_main
            "${QDGZ300_DEPS_PREFIX}"
            gtest_main.lib
            gtest_maind.lib
            libgtest_main.a
            libgtest_maind.a
        )
        qdgz300_create_static_import(
            GTest::gmock
            "${QDGZ300_DEPS_PREFIX}"
            gmock.lib
            gmockd.lib
            libgmock.a
            libgmockd.a
        )
    endif()
endfunction()

function(qdgz300_ensure_spdlog_target)
    if(TARGET spdlog::spdlog)
        return()
    endif()

    if(TARGET spdlog)
        add_library(spdlog::spdlog ALIAS spdlog)
        return()
    endif()

    if(TARGET spdlog_header_only)
        add_library(spdlog::spdlog ALIAS spdlog_header_only)
    endif()
endfunction()

function(qdgz300_ensure_yaml_target)
    if(TARGET yaml-cpp::yaml-cpp)
        return()
    endif()

    if(TARGET yaml-cpp)
        add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
    endif()
endfunction()

function(qdgz300_try_add_local_source dep_dir dep_name binary_dir)
    if(EXISTS "${dep_dir}")
        message(STATUS "Using extracted offline source for ${dep_name}: ${dep_dir}")
        add_subdirectory("${dep_dir}" "${binary_dir}" EXCLUDE_FROM_ALL)
        set(QDGZ300_LOCAL_SOURCE_USED ON PARENT_SCOPE)
    endif()
endfunction()

qdgz300_try_import_from_shared_prefix()

set(QDGZ300_LOCAL_SOURCE_USED OFF)

if(NOT TARGET spdlog::spdlog)
    qdgz300_try_add_local_source(
        "${DEPS_OFFLINE_ROOT}/spdlog-1.13.0"
        "spdlog"
        "${CMAKE_BINARY_DIR}/third_party/spdlog"
    )
    qdgz300_ensure_spdlog_target()
endif()

if(NOT TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    qdgz300_try_add_local_source(
        "${DEPS_OFFLINE_ROOT}/yaml-cpp-0.8.0"
        "yaml-cpp"
        "${CMAKE_BINARY_DIR}/third_party/yaml-cpp"
    )
    qdgz300_ensure_yaml_target()
endif()

if(NOT TARGET spdlog::spdlog)
    message(FATAL_ERROR
        "spdlog target not available. Set QDGZ300_DEPS_PREFIX to a prepared shared prefix "
        "or set DEPS_OFFLINE_ROOT to an extracted offline source directory.")
endif()

if(NOT TARGET yaml-cpp::yaml-cpp)
    message(FATAL_ERROR
        "yaml-cpp target not available. Set QDGZ300_DEPS_PREFIX to a prepared shared prefix "
        "or set DEPS_OFFLINE_ROOT to an extracted offline source directory.")
endif()

if(QDGZ300_ENABLE_PROTOBUF)
    find_package(Protobuf REQUIRED)
    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS "${CMAKE_SOURCE_DIR}/proto/hmi_protocol.proto")
endif()

if(QDGZ300_ENABLE_GPU)
    include("${CMAKE_SOURCE_DIR}/cmake/FindIluvatarCorex.cmake")
endif()

if(QDGZ300_BUILD_TESTING)
    if(NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main OR NOT TARGET GTest::gmock)
        if(EXISTS "${DEPS_OFFLINE_ROOT}/googletest-1.14.0")
            message(STATUS "Using extracted offline source for googletest: ${DEPS_OFFLINE_ROOT}/googletest-1.14.0")
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            add_subdirectory(
                "${DEPS_OFFLINE_ROOT}/googletest-1.14.0"
                "${CMAKE_BINARY_DIR}/third_party/googletest"
                EXCLUDE_FROM_ALL
            )
            if(NOT TARGET GTest::gtest)
                add_library(GTest::gtest ALIAS gtest)
            endif()
            if(NOT TARGET GTest::gtest_main)
                add_library(GTest::gtest_main ALIAS gtest_main)
            endif()
            if(NOT TARGET GTest::gmock)
                add_library(GTest::gmock ALIAS gmock)
            endif()
        endif()
    endif()

    if(NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
        message(FATAL_ERROR
            "GTest targets not available. Set QDGZ300_DEPS_PREFIX to a prepared shared prefix "
            "or set DEPS_OFFLINE_ROOT to an extracted offline source directory.")
    endif()

    find_package(benchmark CONFIG QUIET)
    if(NOT benchmark_FOUND AND EXISTS "${DEPS_OFFLINE_ROOT}/benchmark-1.8.3")
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        add_subdirectory("${DEPS_OFFLINE_ROOT}/benchmark-1.8.3" EXCLUDE_FROM_ALL)
    endif()
endif()

find_package(Boost QUIET)
if(Boost_FOUND)
    message(STATUS "Found Boost: ${Boost_VERSION}")
endif()
