set(TOWERGUARD_SOURCES
    config.cpp
    logger.cpp
    version.cpp
)

qdgz300_collect_existing_sources(TOWERGUARD_AVAILABLE_SOURCES ${TOWERGUARD_SOURCES})

if(TOWERGUARD_AVAILABLE_SOURCES)
    add_library(towerguard_core STATIC ${TOWERGUARD_AVAILABLE_SOURCES})
    target_include_directories(towerguard_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(towerguard_core PUBLIC Threads::Threads)
    target_compile_definitions(towerguard_core PUBLIC TOWERGUARD_VERSION="${PROJECT_VERSION}")
else()
    message(WARNING "Skipping target towerguard_core: no existing sources found.")
endif()
