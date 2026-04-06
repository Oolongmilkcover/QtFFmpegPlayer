cmake_minimum_required(VERSION 3.16)

project(QtPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Multimedia OpenGLWidgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Multimedia OpenGLWidgets)

# ========================== 你的播放器源代码全部加在这里 ==========================
set(PROJECT_SOURCES
    main.cpp
    player.cpp
    player.h
    player.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(QtPlayer
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        demuxthread.h demuxthread.cpp
        decodethread.h decodethread.cpp
        videothread.h videothread.cpp
        audiothread.h audiothread.cpp
        videowidget.h videowidget.cpp
        audioplayer.h audioplayer.cpp
        player.cpp player.h player.ui
        main.cpp
    )
else()
    if(ANDROID)
        add_library(QtPlayer SHARED
            ${PROJECT_SOURCES}
        )
    else()
        add_executable(QtPlayer
            ${PROJECT_SOURCES}
        )
    endif()
endif()

# ========================== FFmpeg 8.1 配置 ==========================
set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1")

target_include_directories(QtPlayer PRIVATE ${FFMPEG_PATH}/include)

find_library(AVCODEC_LIB avcodec PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
find_library(AVFORMAT_LIB avformat PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
find_library(AVUTIL_LIB avutil PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
find_library(SWRESAMPLE_LIB swresample PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)

# ========================== 链接库 ==========================
target_link_libraries(QtPlayer PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Multimedia
    Qt${QT_VERSION_MAJOR}::OpenGLWidgets

    ${AVCODEC_LIB}
    ${AVFORMAT_LIB}
    ${AVUTIL_LIB}
    ${SWRESAMPLE_LIB}
)

# Windows 下链接 OpenGL
if(WIN32)
    target_link_libraries(QtPlayer PRIVATE opengl32)
endif()

# Linux 线程
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(QtPlayer PRIVATE Threads::Threads)
endif()

# ========================== 固定配置 ==========================
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.QtPlayer)
endif()

set_target_properties(QtPlayer PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS QtPlayer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(QtPlayer)
endif()
