# FindBoost模块
# 用于查找Boost库（可选用于Boost.Asio网络I/O）

find_package(Boost 1.65 CONFIG QUIET COMPONENTS system)

if(Boost_FOUND)
    message(STATUS "Found Boost version ${Boost_VERSION}")
    message(STATUS "Boost include dirs: ${Boost_INCLUDE_DIRS}")
    message(STATUS "Boost libraries: ${Boost_LIBRARIES}")
else()
    message(WARNING "Boost not found - some features may be unavailable")
endif()
