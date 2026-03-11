add_library(qdgz300_m04 STATIC
    tcp_server.cpp
    udp_sender.cpp
    hmi_session.cpp
    trackdata_serializer.cpp
    output_throttle.cpp
    gateway_metrics.cpp
)

target_include_directories(qdgz300_m04 PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qdgz300_m04 PUBLIC qdgz300_common)
