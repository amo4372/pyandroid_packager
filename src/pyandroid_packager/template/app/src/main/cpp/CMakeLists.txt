# =============================================================================
# PythonAndroid 精简版 —— 仅包含 Python 嵌入桥接层
#
# 产物：
#   libpython_bridge.so —— Java 通过 JNI 调用的 Python 嵌入桥接层
#   （App 启动即初始化 Python 3.14 并运行 assets/project 里的项目）
# =============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(python_android C CXX)

set(PYTHON_VERSION "3.14" CACHE STRING "嵌入的 Python 版本")

# Python 桥接层：链接 jniLibs 中官方预编译的 libpython3.14.so
set(PYTHON_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libpython${PYTHON_VERSION}.so")
if(NOT EXISTS "${PYTHON_LIB}")
    message(FATAL_ERROR
        "未找到 ${PYTHON_LIB}。请先运行 scripts/prepare.py（或 prepare.sh）"
        "下载并组装官方 Python ${PYTHON_VERSION} Android 预编译包。")
endif()

add_library(python SHARED IMPORTED)
set_target_properties(python PROPERTIES IMPORTED_LOCATION "${PYTHON_LIB}")

set(PY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/python_include")
if(NOT EXISTS "${PY_INCLUDE_DIR}/Python.h")
    message(FATAL_ERROR
        "未找到 ${PY_INCLUDE_DIR}/Python.h。请先运行 scripts/prepare.py。")
endif()

add_library(python_bridge SHARED python_bridge.cpp crash_handler.cpp)
target_include_directories(python_bridge PRIVATE "${PY_INCLUDE_DIR}")
target_link_libraries(python_bridge python log android)

# =============================================================================
# libpreload.so —— 全局符号预加载库（必须在 python_bridge/SDL 之前加载）
#   用 dlopen(RTLD_GLOBAL) 把 libpython3.14.so / SDL 全家放入全局符号表，
#   否则 pygame 扩展 dlopen 时解析不到 Python API 符号而崩溃。
# =============================================================================
add_library(preload SHARED preload.cpp)
target_link_libraries(preload log android)

# =============================================================================
# libmain.so —— SDL 模式（pygame / SDL 项目）入口
#   - 由 org.libsdl.app.SDLActivity 加载；libSDL2.so 的 JNI 启动 SDL 主线程
#     并 dlsym 调用本库导出的 SDL_main()
#   - 复用 python_bridge.cpp 的公共初始化逻辑
# =============================================================================
set(SDL2_JNI_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libSDL2.so")
set(SDL2_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/SDL2/include")
if(EXISTS "${SDL2_JNI_LIB}")
    add_library(main SHARED sdl_main.cpp python_bridge.cpp crash_handler.cpp)
    target_include_directories(main PRIVATE
        "${PY_INCLUDE_DIR}"
        "${SDL2_INC_DIR}"
    )
    target_link_libraries(main python log android "${SDL2_JNI_LIB}")
endif()
