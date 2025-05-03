cmake_minimum_required(VERSION 3.10)
project(video_stream_example)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖包
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)

# 查找GStreamer相关包
pkg_check_modules(GSTREAMER REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)

# 查找JSON库
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# 包含目录
include_directories(
    ${OpenCV_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
)

# 添加可执行文件
add_executable(server
    server.cpp
)

add_executable(client
    client.cpp
)

# 链接库
target_link_libraries(server
    ${OpenCV_LIBS}
    ${GSTREAMER_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    pthread
    rt
)

target_link_libraries(client
    ${OpenCV_LIBS}
    ${GSTREAMER_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    pthread
    rt
)

# 添加GStreamer编译定义
target_compile_options(server PRIVATE ${GSTREAMER_CFLAGS_OTHER})
target_compile_options(client PRIVATE ${GSTREAMER_CFLAGS_OTHER})

# 设置运行时环境变量（可选，用于GStreamer插件路径）
if(APPLE)
    set_target_properties(server PROPERTIES
        MACOSX_RPATH 1
    )
    set_target_properties(client PROPERTIES
        MACOSX_RPATH 1
    )
endif()