# cmake/FindIluvatarCorex.cmake — 查找 Iluvatar CoreX SDK (CUDA 10.2 API 兼容)
#
# 定义 Target:
# IluvatarCorex::Runtime  — GPU 运行时库
#
# 输入变量:
# COREX_SDK_ROOT — CoreX SDK 安装路径 (默认 /opt/iluvatar/corex)
#
# 输出变量:
# IluvatarCorex_FOUND
# IluvatarCorex_INCLUDE_DIRS
# IluvatarCorex_LIBRARIES

if(NOT COREX_SDK_ROOT)
    set(COREX_SDK_ROOT "/opt/iluvatar/corex" CACHE PATH "Iluvatar CoreX SDK root")
endif()

find_path(IluvatarCorex_INCLUDE_DIR
    NAMES cuda_runtime.h
    HINTS ${COREX_SDK_ROOT}/include
    /usr/local/cuda/include
)

find_library(IluvatarCorex_CUDART_LIBRARY
    NAMES cudart
    HINTS ${COREX_SDK_ROOT}/lib64
    ${COREX_SDK_ROOT}/lib
    /usr/local/cuda/lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(IluvatarCorex
    REQUIRED_VARS IluvatarCorex_INCLUDE_DIR IluvatarCorex_CUDART_LIBRARY
)

if(IluvatarCorex_FOUND)
    set(IluvatarCorex_INCLUDE_DIRS ${IluvatarCorex_INCLUDE_DIR})
    set(IluvatarCorex_LIBRARIES ${IluvatarCorex_CUDART_LIBRARY})

    if(NOT TARGET IluvatarCorex::Runtime)
        add_library(IluvatarCorex::Runtime UNKNOWN IMPORTED)
        set_target_properties(IluvatarCorex::Runtime PROPERTIES
            IMPORTED_LOCATION "${IluvatarCorex_CUDART_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IluvatarCorex_INCLUDE_DIR}"
        )
    endif()

    message(STATUS "Found Iluvatar CoreX SDK: ${COREX_SDK_ROOT}")
    message(STATUS "  Include: ${IluvatarCorex_INCLUDE_DIR}")
    message(STATUS "  Library: ${IluvatarCorex_CUDART_LIBRARY}")
else()
    message(STATUS "Iluvatar CoreX SDK not found. GPU support disabled.")
endif()

mark_as_advanced(IluvatarCorex_INCLUDE_DIR IluvatarCorex_CUDART_LIBRARY)
