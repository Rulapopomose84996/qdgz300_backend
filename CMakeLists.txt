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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(cmake/options.cmake)
include(cmake/warnings.cmake)
qdgz300_enable_global_warnings()

# ==============================================================================
# 第三方依赖
# ==============================================================================
include(cmake/CompilerOptions.cmake OPTIONAL)
add_subdirectory(third_party)

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
            set(_candidate "${CMAKE_CURRENT_SOURCE_DIR}/${_source}")
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
# 已实现源码模块
# ==============================================================================
add_subdirectory(src)

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
    add_executable(receiver_simulator tests/unit/simulator.cpp)
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
