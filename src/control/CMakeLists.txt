add_library(qdgz300_control STATIC
    orchestrator.cpp
    state_machine.cpp
    event_dispatcher.cpp
    command_bridge.cpp
    boot_checker.cpp
    runtime_monitor.cpp
    orchestrator_metrics.cpp
)

target_include_directories(qdgz300_control PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qdgz300_control PUBLIC qdgz300_common)
