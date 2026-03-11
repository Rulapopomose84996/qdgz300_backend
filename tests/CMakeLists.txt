# tests — split into stable unit and integration entry points for Kylin V10.

if(NOT TARGET GTest::gtest_main)
    message(WARNING "GTest targets not found in tests/CMakeLists.txt. Ensure third_party/CMakeLists sets them up.")
    return()
endif()

add_subdirectory(unit)
add_subdirectory(integration)

add_test(
    NAME unit_ctest
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}/tests/unit --output-on-failure
)
set_tests_properties(unit_ctest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests/unit
)

add_test(
    NAME integration_ctest
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}/tests/integration --output-on-failure
)
set_tests_properties(integration_ctest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests/integration
)
