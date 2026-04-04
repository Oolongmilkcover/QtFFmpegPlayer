cmake_minimum_required(VERSION 3.16)
project(Play2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 查找 Qt 5.15.2 组件
find_package(Qt5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Multimedia
    OpenGL
)

# FFmpeg 8.1 路径（请根据实际安装位置修改）
set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1")
include_directories(${FFMPEG_PATH}/include)
link_directories(${FFMPEG_PATH}/lib)

# 收集所有源文件
set(PROJECT_SOURCES
    main.cpp
    Play2.cpp
    Demux.cpp
    DemuxThread.cpp
    Decode.cpp
    DecodeThread.cpp
    VideoThread.cpp
    AudioThread.cpp
    Resample.cpp
    AudioPlay.cpp
    Slider.cpp
    VideoWidget.cpp
    Play2.h
    Demux.h
    DemuxThread.h
    Decode.h
    DecodeThread.h
    videothread.h
    audiothread.h
    resample.h
    AudioPlay.h
    Slider.h
    VideoWidget.h
    play2.ui
)

# 生成可执行文件
add_executable(${PROJECT_NAME} ${PROJECT_SOURCES}
    videocall.h)

# 链接 Qt 库
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Multimedia
    Qt5::OpenGL
)

# 链接 FFmpeg 库
target_link_libraries(${PROJECT_NAME} PRIVATE
    avcodec
    avformat
    avutil
    swresample
    swscale
)

# Windows 下额外配置
if(WIN32)
    # 链接 OpenGL 库
    target_link_libraries(${PROJECT_NAME} PRIVATE opengl32 glu32)
    # 控制台输出（调试用）
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
    #修复：MSVC 正确启用 UTF-8 编码
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    endif()
endif()

# Linux 平台配置
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
endif()

message(STATUS "FFmpeg path: ${FFMPEG_PATH}")
message(STATUS "Qt version: ${Qt5_VERSION}")



if(MSVC)
    add_compile_options("/utf-8")
    # 或者使用 target_compile_options
    # target_compile_options(${PROJECT_NAME} PRIVATE "/utf-8")
endif()


