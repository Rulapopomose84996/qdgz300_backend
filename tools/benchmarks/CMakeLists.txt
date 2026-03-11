add_executable(bench_stress stress_test.cpp)
target_link_libraries(bench_stress PRIVATE Threads::Threads)

add_executable(bench_capacity capacity_test.cpp)
target_link_libraries(bench_capacity PRIVATE Threads::Threads)

add_executable(bench_fpga_rx_limit fpga_rx_limit.cpp)
target_include_directories(bench_fpga_rx_limit PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(bench_fpga_rx_limit PRIVATE Threads::Threads)

add_executable(bench_m01_rx_limit m01_rx_limit.cpp)
target_link_libraries(bench_m01_rx_limit PRIVATE receiver_core Threads::Threads)

install(TARGETS bench_stress bench_capacity bench_fpga_rx_limit bench_m01_rx_limit
    RUNTIME DESTINATION bin/benchmarks)

option(QDGZ300_BUILD_LEGACY_BENCHMARKS "Build legacy benchmark binaries" OFF)
if(QDGZ300_BUILD_LEGACY_BENCHMARKS)
    add_executable(bench_throughput throughput.cpp)
    target_link_libraries(bench_throughput PRIVATE receiver_core Threads::Threads)

    add_executable(bench_numa_affinity numa_affinity.cpp)
    target_link_libraries(bench_numa_affinity PRIVATE receiver_core Threads::Threads)
    install(TARGETS bench_throughput bench_numa_affinity
        RUNTIME DESTINATION bin/benchmarks)
endif()
