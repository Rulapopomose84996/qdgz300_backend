# Third-party dependency setup. Prefer project-local offline sources first on Kylin,
# then fall back to system packages only when local sources are unavailable.

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

if(EXISTS "${DEPS_OFFLINE_ROOT}/spdlog-1.13.0")
    message(STATUS "Using project-local offline spdlog source.")
    add_subdirectory(
        "${DEPS_OFFLINE_ROOT}/spdlog-1.13.0"
        "${CMAKE_BINARY_DIR}/third_party/spdlog"
        EXCLUDE_FROM_ALL
    )
    qdgz300_ensure_spdlog_target()
else()
    find_package(spdlog CONFIG QUIET)
    qdgz300_ensure_spdlog_target()
    if(spdlog_FOUND AND TARGET spdlog::spdlog)
        message(STATUS "Found spdlog: system installed version ${spdlog_VERSION}")
    else()
        message(FATAL_ERROR "spdlog target not available, and local source not found at ${DEPS_OFFLINE_ROOT}/spdlog-1.13.0")
    endif()
endif()

if(EXISTS "${DEPS_OFFLINE_ROOT}/yaml-cpp-0.8.0")
    message(STATUS "Using project-local offline yaml-cpp source.")
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${DEPS_OFFLINE_ROOT}/yaml-cpp-0.8.0"
        "${CMAKE_BINARY_DIR}/third_party/yaml-cpp"
        EXCLUDE_FROM_ALL
    )
    qdgz300_ensure_yaml_target()
else()
    find_package(yaml-cpp CONFIG QUIET)
    qdgz300_ensure_yaml_target()
    if(yaml-cpp_FOUND AND TARGET yaml-cpp::yaml-cpp)
        message(STATUS "Found yaml-cpp: system installed version")
    else()
        message(FATAL_ERROR "yaml-cpp target not available, and local source not found at ${DEPS_OFFLINE_ROOT}/yaml-cpp-0.8.0")
    endif()
endif()

if(QDGZ300_ENABLE_PROTOBUF)
    find_package(Protobuf REQUIRED)
    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS "${CMAKE_SOURCE_DIR}/docs/冻结资产/hmi_protocol.proto")
endif()

if(QDGZ300_ENABLE_GPU)
    include("${CMAKE_SOURCE_DIR}/cmake/FindIluvatarCorex.cmake")
endif()

if(QDGZ300_BUILD_TESTING)
    if(EXISTS "${DEPS_OFFLINE_ROOT}/googletest-1.14.0")
        message(STATUS "Using project-local offline googletest source.")
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
    else()
        find_package(GTest CONFIG QUIET)
        if(GTest_FOUND AND TARGET GTest::gtest AND TARGET GTest::gtest_main)
            message(STATUS "Found GTest: system installed version")
        else()
            message(FATAL_ERROR "Local googletest source not found at ${DEPS_OFFLINE_ROOT}/googletest-1.14.0")
        endif()
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
