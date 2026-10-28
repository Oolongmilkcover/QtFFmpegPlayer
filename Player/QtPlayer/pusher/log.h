#ifndef PUSHER_LOG_H
#define PUSHER_LOG_H

/*
 * 无 Qt 的日志层。
 *
 * 接口故意模仿 QDebug（logInfo() << a << b 以及 .nospace()），
 * 这样从 Qt 版本迁移过来时，调用点只需要把 qInfo/qWarning/qCritical 换个名字，其余不用动。
 *
 * 分行输出：info 走 stdout，warn/err 走 stderr（和 Qt 的默认行为一致，方便 2> 单独抓错误）。
 *
 * 注意：这里没做逐行加锁，多个线程同时打日志时单行内容可能交错。
 * 实时链路上日志量不大，先按简单来；真要严格不打岔再给 Line 加一把静态互斥。
 */

#include <cstdio>
#include <string>

namespace pusherlog {

class Line
{
public:
    explicit Line(FILE* out) : m_out(out) {}

    // 临时对象：析构时换行
    // fflush 是必要的：输出重定向到文件/管道时 stdout 是全缓冲，
    // 程序崩溃（或被杀）会丢掉最后几行 —— 而最后几行往往正是出问题的现场。
    ~Line() { std::fputc('\n', m_out); std::fflush(m_out); }

    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;

    // 名字和 QDebug 一致，迁移时不用改
    Line& nospace() { m_space = false; return *this; }

    Line& operator<<(const char* s)
    {
        prefix();
        std::fputs(s ? s : "(null)", m_out);
        return *this;
    }
    Line& operator<<(const std::string& s) { return *this << s.c_str(); }
    Line& operator<<(char c)   { prefix(); std::fputc(c, m_out); return *this; }
    Line& operator<<(bool v)   { prefix(); std::fputs(v ? "true" : "false", m_out); return *this; }
    Line& operator<<(int v)    { prefix(); std::fprintf(m_out, "%d", v); return *this; }
    Line& operator<<(unsigned v) { prefix(); std::fprintf(m_out, "%u", v); return *this; }
    Line& operator<<(long v)   { prefix(); std::fprintf(m_out, "%ld", v); return *this; }
    Line& operator<<(unsigned long v) { prefix(); std::fprintf(m_out, "%lu", v); return *this; }
    Line& operator<<(long long v) { prefix(); std::fprintf(m_out, "%lld", v); return *this; }
    Line& operator<<(unsigned long long v) { prefix(); std::fprintf(m_out, "%llu", v); return *this; }
    Line& operator<<(double v) { prefix(); std::fprintf(m_out, "%g", v); return *this; }
    Line& operator<<(float v)  { prefix(); std::fprintf(m_out, "%g", static_cast<double>(v)); return *this; }

private:
    // 第一个元素直接输出，之后的每个元素前加一个空格（QDebug 的默认行为）
    void prefix()
    {
        if (!m_started) m_started = true;
        else if (m_space) std::fputc(' ', m_out);
    }

    FILE* m_out = stderr;
    bool  m_started = false;
    bool  m_space = true;
};

} // namespace pusherlog

inline pusherlog::Line logInfo() { return pusherlog::Line(stdout); }
inline pusherlog::Line logWarn() { return pusherlog::Line(stderr); }
inline pusherlog::Line logErr()  { return pusherlog::Line(stderr); }

#endif // PUSHER_LOG_H
