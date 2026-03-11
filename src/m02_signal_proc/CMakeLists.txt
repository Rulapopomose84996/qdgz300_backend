set(M02_SOURCES
    gpu_dispatcher.cpp
    cuda_stream_pool.cpp
    gpu_pipeline.cpp
    pinned_buffer_pool.cpp
    signal_proc_metrics.cpp
)

if(QDGZ300_ENABLE_GPU AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/signal_kernels.cu")
    list(APPEND M02_SOURCES signal_kernels.cu)
endif()

qdgz300_collect_existing_sources(M02_AVAILABLE_SOURCES ${M02_SOURCES})

if(M02_AVAILABLE_SOURCES)
    add_library(qdgz300_m02 STATIC ${M02_AVAILABLE_SOURCES})
    target_link_libraries(qdgz300_m02 PUBLIC qdgz300_common)

    if(QDGZ300_ENABLE_GPU AND TARGET IluvatarCorex::Runtime)
        target_link_libraries(qdgz300_m02 PUBLIC IluvatarCorex::Runtime)
        target_compile_definitions(qdgz300_m02 PUBLIC QDGZ300_HAS_GPU=1)
    endif()
else()
    message(WARNING "Skipping target qdgz300_m02: no existing sources found.")
endif()
