#ifndef CORE_LOG_H
#define CORE_LOG_H

/*
 * core 侧的极简日志（纯 C++，不依赖 Qt）
 *
 * 用途：替换原来散落在 core 里的 qDebug()，让 core 不再需要 <QDebug> / QtGlobal。
 *
 * 用法与 qDebug() 完全一致：
 *     logDebug() << "open" << url << "failed!:" << err_buf;
 *     logError() << "resampleInit failed, ret=" << ret;
 *
 * 实现要点：
 *   - 一行日志先在 std::ostringstream 里拼好，析构时整体加锁写出，
 *     避免多个线程（demux / 解码 / 渲染）的 << 互相插进对方半行里；
 *   - 输出到 stderr，和 Qt 的 qDebug 一样不缓冲（fflush）；
 *   - LogLine 不可拷贝，C++17 的强制复制消除保证 logDebug() 返回值直接构造调用点对象。
 */

#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

namespace core
{

inline std::mutex &logMutex()
{
    static std::mutex m;
    return m;
}

class LogLine
{
public:
    explicit LogLine(const char *tag = nullptr)
    {
        if (tag)
            m_buf << tag;
    }

    ~LogLine()
    {
        m_buf << '\n';
        const std::string text = m_buf.str();
        std::lock_guard<std::mutex> lock(logMutex());
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fflush(stderr);
    }

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    template <class T>
    LogLine &operator<<(const T &value)
    {
        m_buf << value;
        return *this;
    }

private:
    std::ostringstream m_buf;
};

} // namespace core

inline core::LogLine logDebug() { return core::LogLine(); }
inline core::LogLine logWarn() { return core::LogLine("[warn] "); }
inline core::LogLine logError() { return core::LogLine("[error] "); }

#endif // CORE_LOG_H
