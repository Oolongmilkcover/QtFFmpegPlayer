# FrameQueue 单元测试：直接链 op_core —— 测的就是产品在用的同一份代码，
# 不再单独编译一份 framequeue.cpp（那样测的和产品会慢慢漂开）。
#
# op_core 零 Qt，所以这里也不再链 Qt6::Core；头文件路径和 FFmpeg 依赖都由
# op_core 的 PUBLIC 依赖自动传过来，不用重复写。
# 不创建窗口、不建 QApplication，因此 headless（CI / 无桌面虚拟机）也能直接跑。

add_executable(test_framequeue
    test_framequeue.cpp
)

target_link_libraries(test_framequeue PRIVATE op_core)

add_test(NAME framequeue COMMAND test_framequeue)

# 防御性：万一以后有人往测试里加了 QtGui 相关的东西，无桌面环境也不会因为
# "找不到平台插件"直接失败
set_tests_properties(framequeue PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
