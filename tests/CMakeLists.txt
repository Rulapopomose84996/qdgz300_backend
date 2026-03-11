# tests/CMakeLists.txt
# Simple definition of tests, reusing dependencies setup in root CMakeLists.txt

if(NOT TARGET GTest::gtest_main)
    message(WARNING "GTest targets not found in tests/CMakeLists.txt. Ensure root CMakeLists sets them up.")
    return()
endif()

include(GoogleTest)

function(qdgz300_register_test NAME)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${NAME}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DISCOVERY_MODE PRE_TEST
        )
    else()
        add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
    endif()
endfunction()

# Helper function to add tests linked to receiver_core (M01 legacy tests)
function(add_receiver_test NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_link_libraries(${NAME}
        PRIVATE
        receiver_core
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
    )
    qdgz300_register_test(${NAME})
endfunction()

# Helper function to add tests linked to qdgz300_common (T0 infrastructure tests)
function(add_common_test NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_common
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_test(${NAME})
endfunction()

function(add_m02_test NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_m02
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_test(${NAME})
endfunction()

function(add_m01_m02_test NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_m01
        qdgz300_m02
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_test(${NAME})
endfunction()

# ═══ T0: 公共基础设施层测试 ═══
add_common_test(common_spsc_queue_tests common_spsc_queue_tests.cpp)
add_common_test(common_memory_pool_tests common_memory_pool_tests.cpp)
add_common_test(common_types_tests common_types_tests.cpp)
add_common_test(common_system_state_tests common_system_state_tests.cpp)
add_common_test(common_metrics_tests common_metrics_tests.cpp)
add_common_test(common_error_codes_tests common_error_codes_tests.cpp)
add_common_test(common_event_types_tests common_event_types_tests.cpp)
add_common_test(common_config_snapshot_tests common_config_snapshot_tests.cpp)

# ═══ M01: Receiver 模块旧版测试 ═══
add_receiver_test(config_manager_tests config_manager_tests.cpp)
add_receiver_test(metrics_tests metrics_tests.cpp)
add_receiver_test(packet_pool_tests packet_pool_tests.cpp)
add_receiver_test(pcap_writer_tests pcap_writer_tests.cpp)
add_receiver_test(reassembler_tests reassembler_tests.cpp)
add_receiver_test(reorderer_tests reorderer_tests.cpp)
add_receiver_test(rawblock_adapter_tests rawblock_adapter_tests.cpp)
add_receiver_test(integration_rawblock_delivery_tests integration_rawblock_delivery_tests.cpp)
add_receiver_test(integration_tests_fpga integration_tests_fpga.cpp)
add_receiver_test(spsc_queue_tests spsc_queue_tests.cpp)
add_receiver_test(stub_consumer_tests stub_consumer_tests.cpp)
add_receiver_test(udp_receiver_tests udp_receiver_tests.cpp)
add_m02_test(gpu_pipeline_inflight_tests gpu_pipeline_inflight_tests.cpp)
add_m02_test(gpu_dispatcher_tests gpu_dispatcher_tests.cpp)
add_m02_test(m02_dispatcher_integration_tests m02_dispatcher_integration_tests.cpp)
add_m02_test(m02_resource_pool_tests m02_resource_pool_tests.cpp)
add_m02_test(m02_signal_proc_tests m02_signal_proc_tests.cpp)
add_m01_m02_test(integration_m01_m02_e2e_tests integration_m01_m02_e2e_tests.cpp)

# Legacy tests depend on removed protocol APIs; keep them disabled for now.
# add_receiver_test(protocol_tests protocol_tests.cpp)
# add_receiver_test(qos_priority_tests qos_priority_tests.cpp)
# add_receiver_test(validator_tests validator_tests.cpp)
# add_receiver_test(integration_tests_m01 integration_tests_m01.cpp)
