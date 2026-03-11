include(GoogleTest)

function(qdgz300_register_unit_test NAME)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${NAME}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DISCOVERY_MODE PRE_TEST
        )
    else()
        add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
    endif()
endfunction()

function(add_receiver_unit_test NAME SOURCE)
    add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    target_link_libraries(${NAME}
        PRIVATE
        receiver_core
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
    )
    qdgz300_register_unit_test(${NAME})
endfunction()

function(add_common_unit_test NAME SOURCE)
    add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_common
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_unit_test(${NAME})
endfunction()

function(add_m02_unit_test NAME SOURCE)
    add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_m02
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_unit_test(${NAME})
endfunction()

function(add_m03_unit_test NAME SOURCE)
    add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_m03
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_unit_test(${NAME})
endfunction()

add_common_unit_test(common_spsc_queue_tests common_spsc_queue_tests.cpp)
add_common_unit_test(common_memory_pool_tests common_memory_pool_tests.cpp)
add_common_unit_test(common_types_tests common_types_tests.cpp)
add_common_unit_test(common_system_state_tests common_system_state_tests.cpp)
add_common_unit_test(common_metrics_tests common_metrics_tests.cpp)
add_common_unit_test(common_error_codes_tests common_error_codes_tests.cpp)
add_common_unit_test(common_event_types_tests common_event_types_tests.cpp)
add_common_unit_test(common_config_snapshot_tests common_config_snapshot_tests.cpp)

add_receiver_unit_test(config_manager_tests config_manager_tests.cpp)
add_receiver_unit_test(metrics_tests metrics_tests.cpp)
add_receiver_unit_test(packet_pool_tests packet_pool_tests.cpp)
add_receiver_unit_test(pcap_writer_tests pcap_writer_tests.cpp)
add_receiver_unit_test(reassembler_tests reassembler_tests.cpp)
add_receiver_unit_test(reorderer_tests reorderer_tests.cpp)
add_receiver_unit_test(rawblock_adapter_tests rawblock_adapter_tests.cpp)
add_receiver_unit_test(spsc_queue_tests spsc_queue_tests.cpp)
add_receiver_unit_test(stub_consumer_tests stub_consumer_tests.cpp)
add_receiver_unit_test(udp_receiver_tests udp_receiver_tests.cpp)

add_m02_unit_test(gpu_pipeline_inflight_tests gpu_pipeline_inflight_tests.cpp)
add_m02_unit_test(gpu_dispatcher_tests gpu_dispatcher_tests.cpp)
add_m02_unit_test(m02_resource_pool_tests m02_resource_pool_tests.cpp)
add_m02_unit_test(m02_signal_proc_tests m02_signal_proc_tests.cpp)
add_m03_unit_test(m03_data_proc_tests m03_data_proc_tests.cpp)

# Disabled until legacy protocol APIs are reintroduced or tests are rewritten.
# add_receiver_unit_test(protocol_tests protocol_tests.cpp)
# add_receiver_unit_test(qos_priority_tests qos_priority_tests.cpp)
# add_receiver_unit_test(validator_tests validator_tests.cpp)
