add_subdirectory(benchmarks)

add_executable(pcap_replay pcap_replay.cpp)
target_link_libraries(pcap_replay PRIVATE receiver_core)

add_executable(pcap_analyze pcap_analyze.cpp)
target_link_libraries(pcap_analyze PRIVATE receiver_core)

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/fpga_emulator/emulator_main.cpp")
    add_executable(fpga_emulator
        fpga_emulator/emulator_main.cpp
        fpga_emulator/packet_generator.cpp
    )
    target_include_directories(fpga_emulator PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tools/fpga_emulator
    )
    target_link_libraries(fpga_emulator PRIVATE qdgz300_m01 qdgz300_common Threads::Threads)
endif()

set(QDGZ300_TOOL_INSTALL_TARGETS pcap_replay pcap_analyze)
if(TARGET fpga_emulator)
    list(APPEND QDGZ300_TOOL_INSTALL_TARGETS fpga_emulator)
endif()

install(TARGETS ${QDGZ300_TOOL_INSTALL_TARGETS} RUNTIME DESTINATION bin)
