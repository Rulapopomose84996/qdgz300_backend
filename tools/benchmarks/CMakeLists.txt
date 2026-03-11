# benches/CMakeLists.txt
# Benchmark executables

add_executable(bench_stress stress_test.cpp)
target_link_libraries(bench_stress PRIVATE Threads::Threads)

add_executable(bench_capacity capacity_test.cpp)
target_link_libraries(bench_capacity PRIVATE Threads::Threads)

add_executable(bench_fpga_rx_limit fpga_rx_limit.cpp)
target_include_directories(bench_fpga_rx_limit PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(bench_fpga_rx_limit PRIVATE Threads::Threads)

add_executable(bench_m01_rx_limit m01_rx_limit.cpp)
target_link_libraries(bench_m01_rx_limit PRIVATE receiver_core Threads::Threads)

add_test(NAME bench_stress COMMAND bench_stress)
add_test(NAME bench_capacity COMMAND bench_capacity)
add_test(NAME bench_fpga_rx_limit COMMAND bench_fpga_rx_limit)
add_test(NAME bench_m01_rx_limit COMMAND bench_m01_rx_limit)

option(BENCH_ENABLE_LEGACY "Build legacy benchmark binaries" OFF)
if(BENCH_ENABLE_LEGACY)
    add_executable(bench_throughput throughput.cpp)
    target_link_libraries(bench_throughput PRIVATE receiver_core Threads::Threads)
    add_test(NAME bench_throughput COMMAND bench_throughput)

    add_executable(bench_numa_affinity numa_affinity.cpp)
    target_link_libraries(bench_numa_affinity PRIVATE receiver_core Threads::Threads)
    add_test(NAME bench_numa_affinity COMMAND bench_numa_affinity)
endif()
