cmake_minimum_required(VERSION 3.16)
project(qdgz300_backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(cmake/options.cmake)
include(cmake/warnings.cmake)
qdgz300_enable_global_warnings()
include(cmake/CompilerOptions.cmake OPTIONAL)
include(cmake/third_party.cmake)

function(qdgz300_collect_existing_sources out_var)
    set(_existing_sources)

    foreach(_source IN LISTS ARGN)
        if(IS_ABSOLUTE "${_source}")
            set(_candidate "${_source}")
        else()
            set(_candidate "${CMAKE_CURRENT_SOURCE_DIR}/${_source}")
        endif()

        if(EXISTS "${_candidate}")
            list(APPEND _existing_sources "${_source}")
        else()
            message(WARNING "Skipping missing source file: ${_source}")
        endif()
    endforeach()

    set(${out_var} "${_existing_sources}" PARENT_SCOPE)
endfunction()

add_subdirectory(src)
install(DIRECTORY config/ DESTINATION etc/qdgz300 OPTIONAL)
add_subdirectory(tools)

if(QDGZ300_BUILD_TESTING)
    add_subdirectory(tests)
    add_subdirectory(tools/benchmarks)
endif()
