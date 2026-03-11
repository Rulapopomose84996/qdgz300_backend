set(CONTROL_SOURCES
    ../orchestrator/orchestrator.cpp
    ../orchestrator/state_machine.cpp
    ../orchestrator/event_dispatcher.cpp
    ../orchestrator/command_bridge.cpp
    ../orchestrator/orchestrator_metrics.cpp
    ../config/config_manager.cpp
    ../config/config_validator.cpp
    ../config/param_meta_table.cpp
    ../time_sync/time_sync_manager.cpp
    ../time_sync/time_quality_mapper.cpp
    ../health/health_check.cpp
    ../health/boot_checker.cpp
    ../health/runtime_monitor.cpp
    ../diagnostics/diagnostics_center.cpp
    ../diagnostics/diagnostics_rules.cpp
    ../recorder/recorder.cpp
    ../recorder/disk_writer.cpp
)

qdgz300_collect_existing_sources(CONTROL_AVAILABLE_SOURCES ${CONTROL_SOURCES})

if(CONTROL_AVAILABLE_SOURCES)
    add_library(qdgz300_control STATIC ${CONTROL_AVAILABLE_SOURCES})
    target_link_libraries(qdgz300_control PUBLIC qdgz300_common yaml-cpp::yaml-cpp)
else()
    message(WARNING "Skipping target qdgz300_control: no existing sources found.")
endif()
