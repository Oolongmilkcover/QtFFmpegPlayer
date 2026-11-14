cmake_minimum_required(VERSION 3.21)

project(QtPlayer
    VERSION 0.1
    DESCRIPTION "A Qt/FFmpeg player with a reusable core and an op_pusher console target"
    LANGUAGES CXX
)

# 源码是 UTF-8（含中文注释），而 Windows 默认代码页是 936(GBK)。
# 链 Qt 的目标能顺带拿到 Qt INTERFACE 选项里的 /utf-8；**不链 Qt 的目标拿不到**（比如剥离 Qt 后的 op_pusher），
# 于是 MSVC 按 GBK 去解 UTF-8 源码：先报 C4819，多字节序列里凑出引号/花括号后解析器跑偏，
# 最终在系统头文件（ucrt/time.h、ctime）里报语法错误 —— 看起来毫不相关。
# 所以在工程级加一次，以后抽 core（P2 的零 Qt 内核）时新目标也不会再踩。
if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 只查找 Qt6 组件。
# 注意：这里刻意不写最低版本（op-core 那边写的是 6.5）。Ubuntu 24.04 的发行版 Qt 是 6.4，
# 写死 6.5 会让 .github/workflows/ci.yml 的 Linux 任务在 find_package 阶段直接失败。
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Multimedia
    OpenGL
    OpenGLWidgets
)

# Qt 官方推荐的项目初始化：打开 AUTOMOC / AUTOUIC，并把 C++ 标准设为 17
qt_standard_project_setup()

# qt_standard_project_setup() 只负责 MOC / UIC，**不开 AUTORCC**
# （见 Qt6CoreMacros.cmake 里的 `foreach(auto_set IN ITEMS MOC UIC)`）。
# 少了这行 resources/res.qrc 不会编译进二进制：图标和 Basic.shader 全丢，
# 运行时不报错但界面一片空白，所以必须显式打开。
set(CMAKE_AUTORCC ON)

# FFmpeg：封装成 ffmpeg_deps INTERFACE 目标（头文件路径 + 库一次性定义）
include(cmake/FFmpeg.cmake)

# ============================ 源码清单（按层） ============================
# app/       Qt 壳：窗口、控制栏、标题栏、播放列表、自定义滑条
set(OP_APP_SOURCES
    app/main.cpp
    app/player.h            app/player.cpp            app/player.ui
    app/ctrlbar.h           app/ctrlbar.cpp           app/ctrlbar.ui
    app/topmenu.h           app/topmenu.cpp           app/topmenu.ui
    app/playlist.h          app/playlist.cpp          app/playlist.ui
    app/myslider.h          app/myslider.cpp
)

# core/      内核：队列、解封装、解码、时钟、同步线程。
#            视频解码/渲染、音频都已改成 std::thread；DemuxThread 暂时仍是 QThread（它还有信号连着 app）。
set(OP_CORE_SOURCES
    core/perfclock.h
    core/iaudiooutput.h
    core/packetqueue.h      core/packetqueue.cpp
    core/framequeue.h       core/framequeue.cpp
    core/decodethread.h     core/decodethread.cpp
    core/demuxthread.h      core/demuxthread.cpp
    core/audiothread.h      core/audiothread.cpp
    core/videodecodethread.h  core/videodecodethread.cpp
    core/videorenderthread.h  core/videorenderthread.cpp
    core/messagequeue.h       core/messagequeue.cpp
    core/messageloopthread.h  core/messageloopthread.cpp
    core/playercore.h         core/playercore.cpp
    core/playercontext.h      core/playercontext.cpp
)

# platform/  平台适配：Qt Multimedia 音频输出、QOpenGLWidget 视频渲染目标
set(OP_PLATFORM_SOURCES
    platform/audioplayer.h  platform/audioplayer.cpp
    platform/videowidget.h  platform/videowidget.cpp
)

# ============================ 播放器可执行文件 ============================
qt_add_executable(QtPlayer
    MANUAL_FINALIZATION
    ${OP_APP_SOURCES}
    ${OP_CORE_SOURCES}
    ${OP_PLATFORM_SOURCES}
    resources/res.qrc

)

# ============================ 链接库 ============================
target_link_libraries(QtPlayer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    ffmpeg_deps
)

# ==================== 把仓库根目录加入 include 路径 ====================
# 源码内的 include 一律写成 "core/xxx.h" / "platform/xxx.h" / "app/xxx.h"，
# AUTOUIC 生成的 ui_*.h 同样会 #include "app/ctrlbar.h" 这类自定义控件头文件，
# 而它们只存在于源码目录，所以从仓库根目录解析。
# GCC/Clang 只搜"当前头文件所在目录 + -I 路径"，不加这行 Linux 上必报
#   ui_player.h:19: fatal error: app/ctrlbar.h: No such file or directory
target_include_directories(QtPlayer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# IDE（Qt Creator / Visual Studio）里按层折叠文件
source_group("app"      FILES ${OP_APP_SOURCES})
source_group("core"     FILES ${OP_CORE_SOURCES})
source_group("platform" FILES ${OP_PLATFORM_SOURCES})

# Linux 线程库
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(QtPlayer PRIVATE Threads::Threads)
endif()

# ============================ 最终化与安装 ============================
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

# ============================ 单元测试（无桌面也能跑） ============================
# 只依赖 Qt6::Core + FFmpeg，不创建任何窗口 / QApplication，
# 所以可以在 headless 环境（CI、无桌面虚拟机）直接执行：
#     cmake --build build && ctest --test-dir build --output-on-failure
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ========================== 推流端：op_pusher（mini-OBS） ==========================
# 类型说明：这是同一个工程里的 console 可执行目标（不是 Qt Widgets 应用，不加 WIN32），
#          界面就是终端、参数走命令行；只链 Qt6::Core（线程/日志/参数解析），FFmpeg 干活。
#          要做本地预览窗口时，再打开下面注释掉的 Widgets/OpenGLWidgets 并复用 platform/videowidget.*。
add_executable(op_pusher
    pusher/main.cpp
    pusher/dropqueue.h
    pusher/pusher.h         pusher/pusher.cpp
    pusher/capturesource.h  pusher/capturesource.cpp
    pusher/encoder.h        pusher/encoder.cpp
    pusher/muxer.h          pusher/muxer.cpp
)
target_include_directories(op_pusher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# op_pusher 已经完全不依赖 Qt：日志用 pusher/log.h，参数解析/计时/CSV/信号处理都是标准库。
# 这也是 P2「core 零 Qt」的第一个可验证成果（dumpbin 里不会出现任何 Qt6*.dll）。
target_link_libraries(op_pusher PRIVATE ffmpeg_deps)
# 以后要做本地预览窗口，再单独给一个小目标链 Qt6::Widgets Qt6::OpenGLWidgets，别污染这个目标

if(UNIX AND NOT APPLE)
    target_link_libraries(op_pusher PRIVATE Threads::Threads)
endif()

source_group("pusher" FILES
    pusher/main.cpp pusher/dropqueue.h
    pusher/pusher.h pusher/pusher.cpp
    pusher/capturesource.h pusher/capturesource.cpp
    pusher/encoder.h pusher/encoder.cpp
    pusher/muxer.h pusher/muxer.cpp
)

# 备注 1：pusher/ 下都是普通 C++ 类（没有 QObject / Q_OBJECT），所以不需要写任何 moc 相关代码；
#        工程级 CMAKE_AUTOMOC 仍会给它建一个 autogen 目标，但里面没有真正属于 pusher 的 moc 代码
#        （mocs_compilation.cpp 是空的），无害。
# 备注 2：pusher 用的是自己的 pusher/dropqueue.h，**不是**播放器的 core/framequeue.* / core/packetqueue.*。
#        原因：契约不同 —— 播放器那两个是"播放时序"的一部分（背压不丢帧 + 历史游标支持逐帧回退），
#        而 pusher 要的是"实时缓冲"（满了丢最旧，或者落盘时阻塞等待）。
#        P2 抽 core 时，把 dropqueue.h 提升成 core/queue.h，再让两边各自按需复用。
