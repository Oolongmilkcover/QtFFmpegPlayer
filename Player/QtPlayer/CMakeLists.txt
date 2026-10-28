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
    find_library(AVDEVICE_LIB avdevice PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVUTIL_LIB avutil PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIB swresample PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(SWSCALE_LIB swscale PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)
    find_library(AVFILTER_LIB avfilter PATHS ${FFMPEG_PATH}/lib NO_DEFAULT_PATH)

    target_link_libraries(ffmpeg_deps INTERFACE
        ${AVCODEC_LIB}
        ${AVFORMAT_LIB}
        ${AVDEVICE_LIB}      # 采集设备（dshow）用；播放器不用它，但推流端要用
        ${AVUTIL_LIB}
        ${SWRESAMPLE_LIB}
        ${SWSCALE_LIB}
        ${AVFILTER_LIB}
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavdevice libavutil libswresample libswscale libavfilter)
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

# ========================== 推流端：op_pusher（mini-OBS） ==========================
# 类型说明：这是同一个工程里的 console 可执行目标（不是 Qt Widgets 应用，不加 WIN32），
#          界面就是终端、参数走命令行；只链 Qt6::Core（线程/日志/参数解析），FFmpeg 干活。
#          要做本地预览窗口时，再打开下面注释掉的 Widgets/OpenGLWidgets 并复用 videowidget.*。
add_executable(op_pusher
    pusher/main.cpp
    pusher/dropqueue.h
    pusher/pusher.h         pusher/pusher.cpp
    pusher/capturesource.h  pusher/capturesource.cpp
    pusher/encoder.h        pusher/encoder.cpp
    pusher/muxer.h          pusher/muxer.cpp
)
target_include_directories(op_pusher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(op_pusher PRIVATE Qt6::Core ffmpeg_deps)
# target_link_libraries(op_pusher PRIVATE Qt6::Widgets Qt6::OpenGLWidgets)   # 本地预览用

if(UNIX AND NOT APPLE)
    target_link_libraries(op_pusher PRIVATE Threads::Threads)
endif()

# 备注 1：pusher/ 下都是普通 C++ 类（没有 QObject / Q_OBJECT），所以不需要写任何 moc 相关代码；
#        工程级 CMAKE_AUTOMOC 仍会给它建一个 autogen 目标，但里面没有真正属于 pusher 的 moc 代码
#        （mocs_compilation.cpp 是空的），无害。
# 备注 2：pusher 用的是自己的 pusher/dropqueue.h，**不是**播放器的 framequeue.* / packetqueue.*。
#        原因：契约不同 —— 播放器那两个是"播放时序"的一部分（背压不丢帧 + 历史游标支持逐帧回退），
#        而 pusher 要的是"实时缓冲"（满了丢最旧，或者落盘时阻塞等待）。
#        P2 抽 core 时，把 dropqueue.h 提升成 core/queue.h，再让两边各自按需复用。
