set(M04_SOURCES
    tcp_server.cpp
    udp_sender.cpp
    hmi_session.cpp
    trackdata_serializer.cpp
    output_throttle.cpp
    gateway_metrics.cpp
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
