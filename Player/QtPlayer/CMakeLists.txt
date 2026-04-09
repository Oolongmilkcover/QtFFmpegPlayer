cmake_minimum_required(VERSION 3.16)

project(QtPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 只查找 Qt6 组件
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Multimedia
)

# ========================== 所有源文件 ==========================
set(PROJECT_SOURCES
    main.cpp
    player.cpp
    player.h
    player.ui
    demuxthread.h demuxthread.cpp
    decodethread.h decodethread.cpp
    videothread.h videothread.cpp
    audiothread.h audiothread.cpp
    videowidget.h videowidget.cpp
    audioplayer.h audioplayer.cpp
    myslider.h myslider.cpp
)

qt_add_executable(QtPlayer
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)

# ========================== FFmpeg 配置 ==========================
set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1")

target_include_directories(QtPlayer PRIVATE ${FFMPEG_PATH}/include)

find_library(AVCODEC_LIB avcodec PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
find_library(AVFORMAT_LIB avformat PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
find_library(AVUTIL_LIB avutil PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
find_library(SWRESAMPLE_LIB swresample PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
find_library(SWSCALE_LIB swscale PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)

# ========================== 链接库 ==========================
target_link_libraries(QtPlayer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia


    ${AVCODEC_LIB}
    ${AVFORMAT_LIB}
    ${AVUTIL_LIB}
    ${SWRESAMPLE_LIB}
    ${SWSCALE_LIB}
)



# Linux 线程库
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(QtPlayer PRIVATE Threads::Threads)
endif()

# ========================== 最终化配置 ==========================
set_target_properties(QtPlayer PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

include(GNUInstallDirs)
install(TARGETS QtPlayer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_finalize_executable(QtPlayer)
