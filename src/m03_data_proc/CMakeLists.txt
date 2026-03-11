add_library(qdgz300_m03 STATIC
    tracker.cpp
    kalman_filter.cpp
    plot_associator.cpp
    fusion_engine.cpp
    track_manager.cpp
    track_frame_builder.cpp
    data_proc_metrics.cpp
)

target_include_directories(qdgz300_m03 PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qdgz300_m03 PUBLIC qdgz300_common)
