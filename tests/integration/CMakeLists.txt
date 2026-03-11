include(GoogleTest)

function(qdgz300_register_integration_test NAME)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${NAME}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DISCOVERY_MODE PRE_TEST
        )
    else()
        add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
    endif()
endfunction()

function(add_receiver_integration_test NAME SOURCE)
    add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    target_link_libraries(${NAME}
        PRIVATE
        receiver_core
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
    )
    qdgz300_register_integration_test(${NAME})
endfunction()

function(add_m02_integration_test NAME SOURCE)
    add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_m02
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_integration_test(${NAME})
endfunction()

function(add_m01_m02_integration_test NAME SOURCE)
    add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
    target_link_libraries(${NAME}
        PRIVATE
        qdgz300_m01
        qdgz300_m02
        GTest::gtest
        GTest::gtest_main
    )
    qdgz300_register_integration_test(${NAME})
endfunction()

add_receiver_integration_test(integration_rawblock_delivery_tests integration_rawblock_delivery_tests.cpp)
add_receiver_integration_test(integration_tests_fpga integration_tests_fpga.cpp)
add_m02_integration_test(m02_dispatcher_integration_tests m02_dispatcher_integration_tests.cpp)
add_m01_m02_integration_test(integration_m01_m02_e2e_tests integration_m01_m02_e2e_tests.cpp)

# Disabled until legacy integration test dependencies are restored.
# add_receiver_integration_test(integration_tests_m01 integration_tests_m01.cpp)
