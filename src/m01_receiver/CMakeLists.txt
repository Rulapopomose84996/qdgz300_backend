set(QDGZ300_M01_SOURCES
    app_init.cpp
    app_run.cpp
    capture/pcap_writer.cpp
    config/config_manager.cpp
    delivery/delivery_interface.cpp
    delivery/rawblock_adapter.cpp
    delivery/stub_consumer.cpp
    monitoring/logger.cpp
    monitoring/metrics.cpp
    network/array_face_receiver.cpp
    network/packet_pool.cpp
    network/udp_receiver.cpp
    pipeline/dispatcher.cpp
    pipeline/reassembler.cpp
    pipeline/reorderer.cpp
    pipeline/rx_stage.cpp
    protocol/crc32c.cpp
    protocol/packet_parser.cpp
    signal_handler.cpp
    protocol/validator.cpp
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
    find_library(QDGZ300_RT_LIBRARY rt)
    if(QDGZ300_RT_LIBRARY)
        target_link_libraries(qdgz300_m01 PUBLIC ${QDGZ300_RT_LIBRARY})
    endif()

    if(NUMA_LIBRARY)
        target_link_libraries(qdgz300_m01 PUBLIC ${NUMA_LIBRARY})
        target_compile_definitions(qdgz300_m01 PUBLIC RECEIVER_HAS_LIBNUMA=1)
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        target_compile_options(qdgz300_m01 PRIVATE -march=armv8-a+crypto)
    endif()
endif()

add_library(receiver_core ALIAS qdgz300_m01)

add_executable(receiver_app main.cpp)
target_link_libraries(receiver_app PRIVATE receiver_core)
install(TARGETS receiver_app RUNTIME DESTINATION bin)
