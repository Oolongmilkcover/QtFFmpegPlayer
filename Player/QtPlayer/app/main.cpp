#include "app/player.h"

#include <QApplication>

#ifdef _WIN32
#  include <windows.h>
#  include <cstdio>
#endif

/*
 * 目的：让 qDebug 在"直接运行"（Ctrl+R，不带调试器）时也能看到。
 *
 * 背景：Windows 上 GUI 子系统程序没有控制台，Qt 默认把 qDebug 写成 OutputDebugString，
 * 只有调试器或 DebugView 能接到 —— 所以 Ctrl+R 运行时 Qt Creator 会提示"无法获取调试输出"，
 * 而 F5 调试时（调试器接住了 OutputDebugString）日志又正常。
 *
 * 两步处理：
 *   1) 把 stdout/stderr 接回**父进程的控制台**（从 cmd 启动时有效，日志直接打在 cmd 里）；
 *   2) 设 QT_FORCE_STDERR_LOGGING=1：没有控制台时也强制写 stderr。
 *      Qt Creator 是用管道抓 stderr 的，所以 Application Output 里就能看到日志。
 *
 * 注意：启动前设这个环境变量比在 Qt Creator 的 Run Environment 里配更省事，
 *       而且对 cmd、Qt Creator、将来 CI 都生效。
 */
int main(int argc, char *argv[])
{
#ifdef _WIN32
    // ATTACH_PARENT_PROCESS：如果父进程（cmd / 终端）有控制台，就直接复用它
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }
#endif
    qputenv("QT_FORCE_STDERR_LOGGING", "1");

    QApplication a(argc, argv);
    Player w;
    w.show();
    return a.exec();
}
