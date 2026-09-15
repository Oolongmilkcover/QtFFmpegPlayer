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
    OpenGL
    OpenGLWidgets
)

# ========================== 所有源文件 ==========================
set(PROJECT_SOURCES
    main.cpp
    player.h       player.cpp        player.ui
    demuxthread.h  demuxthread.cpp
    decodethread.h decodethread.cpp
    videodecodethread.h  videodecodethread.cpp
    audiothread.h  audiothread.cpp
    videowidget.h  videowidget.cpp
    audioplayer.h  audioplayer.cpp
    myslider.h     myslider.cpp
    ctrlbar.h      ctrlbar.cpp        ctrlbar.ui
    topmenu.h      topmenu.cpp        topmenu.ui
    playlist.h     playlist.cpp       playlist.ui
)

qt_add_executable(QtPlayer
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
    res.qrc
    ctrlbar.h ctrlbar.cpp ctrlbar.ui
    topmenu.h topmenu.cpp topmenu.ui
    playlist.h playlist.cpp playlist.ui

    packetqueue.h packetqueue.cpp
    framequeue.h framequeue.cpp
    videorenderthread.h videorenderthread.cpp
    perfclock.h

)

# ========================== 链接库 ==========================
target_link_libraries(QtPlayer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::OpenGLWidgets
)

# ==================== 把源码目录加入 include 路径 ====================
# AUTOUIC 生成的 ui_*.h 里会 #include "ctrlbar.h" 这类自定义控件头文件，
# 而它们只存在于源码目录。
# MSVC 对 #include "..." 会沿"包含链"回溯目录（player.cpp 在源码目录 → 能找到），
# GCC/Clang 只会搜"当前头文件所在目录 + -I 路径"，不加这行 Linux 上必报
#   ui_player.h:19: fatal error: ctrlbar.h: No such file or directory
target_include_directories(QtPlayer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ========================== FFmpeg 配置 ==========================
# 统一封装成一个 INTERFACE 目标，主程序与所有测试目标共用。
# 这样"头文件路径 + 库"是一次性定义的，新加 target 只要链接 ffmpeg_deps 就行，
# 不会出现"忘了加 include 目录 → C1083: libavformat/avformat.h: No such file"。
add_library(ffmpeg_deps INTERFACE)

if(WIN32)
    set(FFMPEG_PATH "C:/Program Files/ffmpeg/ffmpeg8.1" CACHE PATH "FFmpeg 安装目录（需含 include/、lib/、bin/），可用 -DFFMPEG_PATH=... 覆盖")

    target_include_directories(ffmpeg_deps INTERFACE ${FFMPEG_PATH}/include)

    find_library(AVCODEC_LIB avcodec PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVFORMAT_LIB avformat PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVUTIL_LIB avutil PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIB swresample PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWSCALE_LIB swscale PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVFILTER_LIB avfilter PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)

    target_link_libraries(ffmpeg_deps INTERFACE
        ${AVCODEC_LIB}
        ${AVFORMAT_LIB}
        ${AVUTIL_LIB}
        ${SWRESAMPLE_LIB}
        ${SWSCALE_LIB}
        ${AVFILTER_LIB}
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswresample libswscale libavfilter)
    # PkgConfig::FFMPEG 自带 include 路径与链接参数
    target_link_libraries(ffmpeg_deps INTERFACE PkgConfig::FFMPEG)
endif()

target_link_libraries(QtPlayer PRIVATE ffmpeg_deps)


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

# ========================== 单元测试（无桌面也能跑） ==========================
# 只依赖 Qt6::Core + FFmpeg，不创建任何窗口 / QApplication，
# 所以可以在 headless 环境（CI、无桌面虚拟机）直接执行：
#     QT_QPA_PLATFORM=offscreen ctest --output-on-failure
enable_testing()

add_executable(test_framequeue
    tests/test_framequeue.cpp
    framequeue.h framequeue.cpp
)
target_include_directories(test_framequeue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_framequeue PRIVATE Qt6::Core ffmpeg_deps)

add_test(NAME framequeue COMMAND test_framequeue)
set_tests_properties(framequeue PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
