cmake_minimum_required(VERSION 3.16)
project(qdgz300_backend VERSION 1.0.0 LANGUAGES CXX)

# ==============================================================================
# 全局设置
# ==============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

option(ENABLE_COVERAGE "Enable code coverage (GCC/Clang only)" OFF)
option(BUILD_SIMULATOR "Build simulator executable" OFF)
option(ENABLE_GPU "Enable GPU (Iluvatar CoreX) support" OFF)
option(ENABLE_PROTOBUF "Enable Protobuf for M04 Gateway" OFF)
include(CTest)

# ==============================================================================
# 交叉编译 / 编译选项
# ==============================================================================
if(CMAKE_CROSSCOMPILING)
    if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake")
        message(STATUS "Cross-compiling for aarch64 with existing toolchain file")
    endif()
endif()

include(cmake/CompilerOptions.cmake OPTIONAL)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)

    if(ENABLE_COVERAGE)
        add_compile_options(-O0 -g -fprofile-arcs -ftest-coverage)
        add_link_options(-fprofile-arcs -ftest-coverage)
    endif()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "This project is designed for Linux. Current system: ${CMAKE_SYSTEM_NAME}")
endif()

# ==============================================================================
# 第三方依赖 (System or Local Offline)
# ==============================================================================
set(DEPS_OFFLINE_ROOT "${CMAKE_SOURCE_DIR}/deps_offline/extracted")

# --- spdlog ---
find_package(spdlog CONFIG QUIET)

if(spdlog_FOUND)
    message(STATUS "Found spdlog: system installed version ${spdlog_VERSION}")
else()
    message(STATUS "spdlog not found on system. Building from local offline source.")

    if(EXISTS "${DEPS_OFFLINE_ROOT}/spdlog-1.13.0")
        add_subdirectory("${DEPS_OFFLINE_ROOT}/spdlog-1.13.0" EXCLUDE_FROM_ALL)

        if(NOT TARGET spdlog::spdlog)
            add_library(spdlog::spdlog ALIAS spdlog)
        endif()
    else()
        message(FATAL_ERROR "Local spdlog source not found at ${DEPS_OFFLINE_ROOT}/spdlog-1.13.0")
    endif()
endif()

# --- yaml-cpp ---
find_package(yaml-cpp CONFIG QUIET)

if(yaml-cpp_FOUND)
    message(STATUS "Found yaml-cpp: system installed version")
else()
    message(STATUS "yaml-cpp not found on system. Building from local offline source.")

    if(EXISTS "${DEPS_OFFLINE_ROOT}/yaml-cpp-0.8.0")
        set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${DEPS_OFFLINE_ROOT}/yaml-cpp-0.8.0" EXCLUDE_FROM_ALL)

        if(NOT TARGET yaml-cpp::yaml-cpp AND TARGET yaml-cpp)
            add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
        endif()
    else()
        message(FATAL_ERROR "Local yaml-cpp source not found at ${DEPS_OFFLINE_ROOT}/yaml-cpp-0.8.0")
    endif()
endif()

# --- Protobuf (optional, for M04 Gateway) ---
if(ENABLE_PROTOBUF)
    find_package(Protobuf REQUIRED)
    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/hmi_protocol.proto)
endif()

# --- Iluvatar CoreX SDK (optional, for M02 GPU) ---
if(ENABLE_GPU)
    include(cmake/FindIluvatarCorex.cmake OPTIONAL)
endif()

# --- GoogleTest (testing only) ---
if(BUILD_TESTING)
    find_package(GTest CONFIG QUIET)

    if(GTest_FOUND)
        message(STATUS "Found GTest: system installed version")
    else()
        message(STATUS "GTest not found on system. Building from local offline source.")

        if(EXISTS "${DEPS_OFFLINE_ROOT}/googletest-1.14.0")
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            add_subdirectory("${DEPS_OFFLINE_ROOT}/googletest-1.14.0" EXCLUDE_FROM_ALL)

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
            message(FATAL_ERROR "Local googletest source not found at ${DEPS_OFFLINE_ROOT}/googletest-1.14.0")
        endif()
    endif()
endif()

# --- Google Benchmark (optional) ---
if(BUILD_TESTING)
    find_package(benchmark CONFIG QUIET)

    if(NOT benchmark_FOUND)
        if(EXISTS "${DEPS_OFFLINE_ROOT}/benchmark-1.8.3")
            set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
            add_subdirectory("${DEPS_OFFLINE_ROOT}/benchmark-1.8.3" EXCLUDE_FROM_ALL)
        endif()
    endif()
endif()

# --- Boost (optional) ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(Boost QUIET)

if(Boost_FOUND)
    message(STATUS "Found Boost: ${Boost_VERSION}")
endif()

# ==============================================================================
# Linux 系统库检测 (libnuma, librt, libdl)
# ==============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckIncludeFileCXX)

    find_library(RT_LIBRARY rt)
    find_library(DL_LIBRARY dl)

    check_include_file_cxx("numa.h" HAS_NUMA_H)

    if(HAS_NUMA_H)
        find_library(NUMA_LIBRARY numa)
    endif()

    find_package(Threads REQUIRED)
endif()

function(qdgz300_collect_existing_sources out_var)
    set(_existing_sources)

    foreach(_source IN LISTS ARGN)
        if(IS_ABSOLUTE "${_source}")
            set(_candidate "${_source}")
        else()
            set(_candidate "${CMAKE_SOURCE_DIR}/${_source}")
        endif()

        if(EXISTS "${_candidate}")
            list(APPEND _existing_sources "${_source}")
        else()
            message(WARNING "Skipping missing source file: ${_source}")
        endif()
    endforeach()

    set(${out_var} "${_existing_sources}" PARENT_SCOPE)
endfunction()

# ==============================================================================
# 库 Target 定义 — 全系统 6 个静态库
# ==============================================================================

# ────────────────────────────────────────────────────────
# qdgz300_common — 公共类型 / 队列 / 内存池 / 指标 / 日志
# ────────────────────────────────────────────────────────
add_library(qdgz300_common STATIC
    src/common/numa_utils.cpp
    src/common/metrics.cpp
    src/common/system_state.cpp
    src/common/error_codes.cpp
    src/logging/async_logger.cpp
    src/logging/metrics_exporter.cpp
)
target_include_directories(qdgz300_common PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qdgz300_common PUBLIC Threads::Threads spdlog::spdlog)

if(NUMA_LIBRARY)
    target_link_libraries(qdgz300_common PUBLIC ${NUMA_LIBRARY})
    target_compile_definitions(qdgz300_common PUBLIC QDGZ300_HAS_LIBNUMA=1)
endif()

# ARM64 optimization
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(qdgz300_common PRIVATE -march=armv8-a+crypto)
endif()

# ────────────────────────────────────────────────────────
# qdgz300_m01 — M01 Receiver 接收模块
# 直接使用现有 receiver_app 项目代码（include/receiver/ + src/）
# ────────────────────────────────────────────────────────
set(QDGZ300_M01_SOURCES
    src/m01_receiver/app_init.cpp
    src/m01_receiver/app_run.cpp
    src/m01_receiver/capture/pcap_writer.cpp
    src/m01_receiver/config/config_manager.cpp
    src/m01_receiver/delivery/delivery_interface.cpp
    src/m01_receiver/delivery/rawblock_adapter.cpp
    src/m01_receiver/delivery/stub_consumer.cpp
    src/m01_receiver/monitoring/logger.cpp
    src/m01_receiver/monitoring/metrics.cpp
    src/m01_receiver/network/array_face_receiver.cpp
    src/m01_receiver/network/packet_pool.cpp
    src/m01_receiver/network/udp_receiver.cpp
    src/m01_receiver/pipeline/dispatcher.cpp
    src/m01_receiver/pipeline/reassembler.cpp
    src/m01_receiver/pipeline/reorderer.cpp
    src/m01_receiver/pipeline/rx_stage.cpp
    src/m01_receiver/protocol/crc32c.cpp
    src/m01_receiver/protocol/packet_parser.cpp
    src/m01_receiver/signal_handler.cpp
    src/m01_receiver/protocol/validator.cpp
)

add_library(qdgz300_m01 STATIC ${QDGZ300_M01_SOURCES})
target_include_directories(qdgz300_m01 PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/qdgz300/m01_receiver
)
target_link_libraries(qdgz300_m01
    PUBLIC qdgz300_common Threads::Threads
    PRIVATE spdlog::spdlog yaml-cpp::yaml-cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(RT_LIBRARY)
        target_link_libraries(qdgz300_m01 PUBLIC ${RT_LIBRARY})
    endif()

    if(NUMA_LIBRARY)
        target_link_libraries(qdgz300_m01 PUBLIC ${NUMA_LIBRARY})
        target_compile_definitions(qdgz300_m01 PUBLIC RECEIVER_HAS_LIBNUMA=1)
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        target_compile_options(qdgz300_m01 PRIVATE -march=armv8-a+crypto)
    endif()
endif()

# 为旧版测试/工具兼容，创建 receiver_core 别名
add_library(receiver_core ALIAS qdgz300_m01)

# ────────────────────────────────────────────────────────
# qdgz300_m02 — M02 SignalProc GPU 信号处理
# ────────────────────────────────────────────────────────
set(M02_SOURCES
    src/m02_signal_proc/gpu_dispatcher.cpp
    src/m02_signal_proc/cuda_stream_pool.cpp
    src/m02_signal_proc/gpu_pipeline.cpp
    src/m02_signal_proc/pinned_buffer_pool.cpp
    src/m02_signal_proc/signal_proc_metrics.cpp
)

# CUDA kernel is compiled only when GPU is enabled
if(ENABLE_GPU)
    list(APPEND M02_SOURCES src/m02_signal_proc/signal_kernels.cu)
endif()

qdgz300_collect_existing_sources(M02_AVAILABLE_SOURCES ${M02_SOURCES})

if(M02_AVAILABLE_SOURCES)
    add_library(qdgz300_m02 STATIC ${M02_AVAILABLE_SOURCES})
    target_link_libraries(qdgz300_m02 PUBLIC qdgz300_common)

    if(ENABLE_GPU AND TARGET IluvatarCorex::Runtime)
        target_link_libraries(qdgz300_m02 PUBLIC IluvatarCorex::Runtime)
        target_compile_definitions(qdgz300_m02 PUBLIC QDGZ300_HAS_GPU=1)
    endif()
else()
    message(WARNING "Skipping target qdgz300_m02: no existing sources found.")
endif()

# ────────────────────────────────────────────────────────
# qdgz300_m03 — M03 DataProc 航迹处理
# ────────────────────────────────────────────────────────
set(M03_SOURCES
    src/m03_data_proc/tracker.cpp
    src/m03_data_proc/kalman_filter.cpp
    src/m03_data_proc/plot_associator.cpp
    src/m03_data_proc/fusion_engine.cpp
    src/m03_data_proc/track_manager.cpp
    src/m03_data_proc/track_frame_builder.cpp
    src/m03_data_proc/data_proc_metrics.cpp
)

qdgz300_collect_existing_sources(M03_AVAILABLE_SOURCES ${M03_SOURCES})

if(M03_AVAILABLE_SOURCES)
    add_library(qdgz300_m03 STATIC ${M03_AVAILABLE_SOURCES})
    target_link_libraries(qdgz300_m03 PUBLIC qdgz300_common)
else()
    message(WARNING "Skipping target qdgz300_m03: no existing sources found.")
endif()

# ────────────────────────────────────────────────────────
# qdgz300_m04 — M04 Gateway 网关
# ────────────────────────────────────────────────────────
set(M04_SOURCES
    src/m04_gateway/tcp_server.cpp
    src/m04_gateway/udp_sender.cpp
    src/m04_gateway/hmi_session.cpp
    src/m04_gateway/trackdata_serializer.cpp
    src/m04_gateway/output_throttle.cpp
    src/m04_gateway/gateway_metrics.cpp
)

if(ENABLE_PROTOBUF)
    list(APPEND M04_SOURCES ${PROTO_SRCS})
endif()

qdgz300_collect_existing_sources(M04_AVAILABLE_SOURCES ${M04_SOURCES})

if(M04_AVAILABLE_SOURCES)
    add_library(qdgz300_m04 STATIC ${M04_AVAILABLE_SOURCES})
    target_link_libraries(qdgz300_m04 PUBLIC qdgz300_common)

    if(ENABLE_PROTOBUF)
        target_link_libraries(qdgz300_m04 PUBLIC protobuf::libprotobuf)
        target_include_directories(qdgz300_m04 PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    endif()
else()
    message(WARNING "Skipping target qdgz300_m04: no existing sources found.")
endif()

# ────────────────────────────────────────────────────────
# qdgz300_control — 控制面全集
# orchestrator / config / time_sync / health /
# diagnostics / recorder
# ────────────────────────────────────────────────────────
set(CONTROL_SOURCES
    src/control/orchestrator/orchestrator.cpp
    src/control/orchestrator/state_machine.cpp
    src/control/orchestrator/event_dispatcher.cpp
    src/control/orchestrator/command_bridge.cpp
    src/control/orchestrator/orchestrator_metrics.cpp
    src/control/config/config_manager.cpp
    src/control/config/config_validator.cpp
    src/control/config/param_meta_table.cpp
    src/control/time_sync/time_sync_manager.cpp
    src/control/time_sync/time_quality_mapper.cpp
    src/control/health/health_check.cpp
    src/control/health/boot_checker.cpp
    src/control/health/runtime_monitor.cpp
    src/control/diagnostics/diagnostics_center.cpp
    src/control/diagnostics/diagnostics_rules.cpp
    src/control/recorder/recorder.cpp
    src/control/recorder/disk_writer.cpp
)

qdgz300_collect_existing_sources(CONTROL_AVAILABLE_SOURCES ${CONTROL_SOURCES})

if(CONTROL_AVAILABLE_SOURCES)
    add_library(qdgz300_control STATIC ${CONTROL_AVAILABLE_SOURCES})
    target_link_libraries(qdgz300_control PUBLIC qdgz300_common yaml-cpp::yaml-cpp)
else()
    message(WARNING "Skipping target qdgz300_control: no existing sources found.")
endif()

# ────────────────────────────────────────────────────────
# towerguard_core — 守护进程核心库
# ────────────────────────────────────────────────────────
set(TOWERGUARD_SOURCES
    src/towerguard/config.cpp
    src/towerguard/logger.cpp
    src/towerguard/version.cpp
)

qdgz300_collect_existing_sources(TOWERGUARD_AVAILABLE_SOURCES ${TOWERGUARD_SOURCES})

if(TOWERGUARD_AVAILABLE_SOURCES)
    add_library(towerguard_core STATIC ${TOWERGUARD_AVAILABLE_SOURCES})
    target_include_directories(towerguard_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(towerguard_core PUBLIC Threads::Threads)
    target_compile_definitions(towerguard_core PUBLIC TOWERGUARD_VERSION="${PROJECT_VERSION}")
else()
    message(WARNING "Skipping target towerguard_core: no existing sources found.")
endif()

# receiver_core 已作为 qdgz300_m01 的 ALIAS 定义，无需重复声明

# ==============================================================================
# 可执行文件
# ==============================================================================

# --- 全系统主程序 ---
if(EXISTS "${CMAKE_SOURCE_DIR}/src/main_backend.cpp"
    AND TARGET qdgz300_m01
    AND TARGET qdgz300_m02
    AND TARGET qdgz300_m03
    AND TARGET qdgz300_m04
    AND TARGET qdgz300_control)
    add_executable(qdgz300_backend src/main_backend.cpp)
    target_link_libraries(qdgz300_backend PRIVATE
        qdgz300_m01 qdgz300_m02 qdgz300_m03 qdgz300_m04 qdgz300_control
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(DL_LIBRARY)
            target_link_libraries(qdgz300_backend PRIVATE ${DL_LIBRARY})
        endif()

        if(RT_LIBRARY)
            target_link_libraries(qdgz300_backend PRIVATE ${RT_LIBRARY})
        endif()
    endif()
else()
    message(WARNING "Skipping executable qdgz300_backend: missing sources or dependent module targets.")
endif()

# --- 旧版 receiver_app (M01 standalone) ---
add_executable(receiver_app src/m01_receiver/main.cpp)
target_link_libraries(receiver_app PRIVATE receiver_core)

# --- 工具 ---
add_executable(pcap_replay tools/pcap_replay.cpp)
target_link_libraries(pcap_replay PRIVATE receiver_core)

add_executable(pcap_analyze tools/pcap_analyze.cpp)
target_link_libraries(pcap_analyze PRIVATE receiver_core)

# --- TowerGuard 守护进程 ---
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/towerguard/towerguard_main.cpp")
    add_executable(towerguard
        tools/towerguard/towerguard_main.cpp
        tools/towerguard/process_monitor.cpp
        tools/towerguard/auto_restart.cpp
    )
    target_include_directories(towerguard PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(towerguard PRIVATE spdlog::spdlog yaml-cpp::yaml-cpp Threads::Threads)
endif()

# --- FPGA 模拟器 (测试注入) ---
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/fpga_emulator/emulator_main.cpp")
    add_executable(fpga_emulator
        tools/fpga_emulator/emulator_main.cpp
        tools/fpga_emulator/packet_generator.cpp
    )
    target_include_directories(fpga_emulator PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tools/fpga_emulator
    )
    target_link_libraries(fpga_emulator PRIVATE qdgz300_m01 qdgz300_common Threads::Threads)
endif()

# --- Simulator (Optional) ---
if(BUILD_SIMULATOR)
    add_executable(receiver_simulator tests/simulator.cpp)
    target_include_directories(receiver_simulator PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(receiver_simulator PRIVATE receiver_core Threads::Threads)
endif()

# ==============================================================================
# Installation
# ==============================================================================
set(QDGZ300_RUNTIME_INSTALL_TARGETS)

foreach(_runtime_target qdgz300_backend receiver_app pcap_replay pcap_analyze)
    if(TARGET ${_runtime_target})
        list(APPEND QDGZ300_RUNTIME_INSTALL_TARGETS ${_runtime_target})
    endif()
endforeach()

if(QDGZ300_RUNTIME_INSTALL_TARGETS)
    install(TARGETS ${QDGZ300_RUNTIME_INSTALL_TARGETS} RUNTIME DESTINATION bin)
endif()

install(DIRECTORY configs/ DESTINATION etc/qdgz300)
install(DIRECTORY config/ DESTINATION etc/qdgz300 OPTIONAL)

# ==============================================================================
# Testing
# ==============================================================================
if(BUILD_TESTING)
    add_subdirectory(tests)
    add_subdirectory(benches)
endif()
