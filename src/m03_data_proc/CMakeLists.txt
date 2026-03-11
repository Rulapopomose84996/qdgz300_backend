set(M03_SOURCES
    tracker.cpp
    kalman_filter.cpp
    plot_associator.cpp
    fusion_engine.cpp
    track_manager.cpp
    track_frame_builder.cpp
    data_proc_metrics.cpp
)

qdgz300_collect_existing_sources(M03_AVAILABLE_SOURCES ${M03_SOURCES})

if(M03_AVAILABLE_SOURCES)
    add_library(qdgz300_m03 STATIC ${M03_AVAILABLE_SOURCES})
    target_link_libraries(qdgz300_m03 PUBLIC qdgz300_common)
else()
    message(WARNING "Skipping target qdgz300_m03: no existing sources found.")
endif()
