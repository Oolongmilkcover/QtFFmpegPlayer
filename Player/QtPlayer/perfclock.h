#ifndef PERFCLOCK_H
#define PERFCLOCK_H

#include <chrono>
#include <QtGlobal>
#include <QDebug>
/*
 * 进程内统一的纳秒时间源。
 *
 * 用 std::chrono::steady_clock（单调时钟，不受系统时间调整影响），
 * origin 是函数内 static 局部变量，全程序只有一份，
 * 因此不同线程 / 不同翻译单元取到的值可以直接相减。
 *
 * C++11 起静态局部变量初始化是线程安全的，可直接多线程调用。
 */

inline qint64 perfNowNs()
{
    static const auto origin = std::chrono::steady_clock::now();
    qint64 tmp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - origin).count();
    return tmp;

}


#endif // PERFCLOCK_H
