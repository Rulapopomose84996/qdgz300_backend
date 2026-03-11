add_library(qdgz300_common STATIC
    numa_utils.cpp
    metrics.cpp
    system_state.cpp
    error_codes.cpp
    ../logging/async_logger.cpp
    ../logging/metrics_exporter.cpp
)

target_include_directories(qdgz300_common PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qdgz300_common PUBLIC Threads::Threads spdlog::spdlog)

if(NUMA_LIBRARY)
    target_link_libraries(qdgz300_common PUBLIC ${NUMA_LIBRARY})
    target_compile_definitions(qdgz300_common PUBLIC QDGZ300_HAS_LIBNUMA=1)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(qdgz300_common PRIVATE -march=armv8-a+crypto)
endif()
