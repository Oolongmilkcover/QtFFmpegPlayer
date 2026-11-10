# FrameQueue 单元测试：只链接 Qt6::Core + FFmpeg，不创建窗口，
# 因此 headless（CI / 无桌面虚拟机）也能直接跑。

add_executable(test_framequeue
    test_framequeue.cpp
    ${PROJECT_SOURCE_DIR}/core/framequeue.cpp
)

target_include_directories(test_framequeue PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_framequeue PRIVATE Qt6::Core ffmpeg_deps)

add_test(NAME framequeue COMMAND test_framequeue)
set_tests_properties(framequeue PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
