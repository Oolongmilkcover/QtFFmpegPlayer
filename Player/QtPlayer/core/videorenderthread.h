#ifndef VIDEORENDERTHREAD_H
#define VIDEORENDERTHREAD_H

#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include "core/framequeue.h"

class VideoWidget;
/*
 * 渲染线程：按音频时钟把解码好的帧送上屏。
 *
 * 它不再继承 QThread（P2：core 去 QThread）：
 * 线程本体由自己持有的 std::thread 管理，start()/stop() 成对，
 * isRunning() 语义同原来的 QThread::isRunning()。
 */
class VideoRenderThread
{
private:
    /*
     * 循环基准时钟（取代 QElapsedTimer）
     *
     * steady_clock 是单调时钟，不受系统时间调整影响，正好对应 QElapsedTimer 的语义；
     * 原来 start()/invalidate() 的成对调用收成 restartLoopTimer() 一个动作。
     */
    using LoopClock = std::chrono::steady_clock;
    LoopClock::time_point m_loopStart = LoopClock::now();
    bool firstFrame = true;

    void restartLoopTimer() { m_loopStart = LoopClock::now(); }

    long long loopElapsedMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   LoopClock::now() - m_loopStart)
            .count();
    }

public:

    explicit VideoRenderThread(FrameQueue* frameQueue);
    ~VideoRenderThread();

    using PaintFrame = std::function<void(AVFrame* frame)>;
    void setPaintFrameCallBack(PaintFrame painter);


    //设置视频帧率
    void setFps(double fps);
    //取视频帧率
    double getFps() const { return m_fps.load(); }

    // 设置倍速
    void setSpeed(double speed);
    /*
     * 外部音频时钟
     *
     * AudioThread负责不断更新它
     *
     * VideoRenderThread只读取
     */
    std::atomic<long long> synpts = -1;
    std::atomic<bool> hasAudio{true};
    //音频是否已经结束
    std::atomic<bool> lastSome = false;
    std::atomic<int> serial = 0;
    std::atomic<long long> pts = 0;
    void setPause(bool isPause);
    void setSerial(int serial);
    void stop();
    void resetFrameClock();
    void restart();
    // 创建渲染线程（std::thread）；stop() 负责 join
    void start();
    bool isRunning() const { return m_running.load(); }

    std::atomic<bool> playDone = false;

    //逐帧请求：>0 表示还有多少次“下一帧”，<0 表示还有多少次“上一帧”
    void requestStep(int delta);
    void clearStepRequests();




protected:

    void run();

private:

    void sleepUntil(int64_t targetWallMs);

    void renderFrame(Frame* frame);

private:

    //VideoWidget* m_widget = nullptr;

    PaintFrame m_painter = nullptr;

    FrameQueue* m_frameQueue = nullptr;

    /*
     * fps / 帧时长：setFps()/setSpeed() 在消息循环线程调用，run() 在渲染线程里读，
     * 所以必须是原子量。原来是裸 double —— 实打实的数据竞争（一边写一边读，
     * 而且这两个值直接决定每帧睡多久）。
     */
    std::atomic<double> m_fps{60.0};

    int m_maxFrameDrop = 5;

    long long  m_lastAudio_pts = 0;

    // 倍速（视频帧时长 = 1000/fps/speed）
    std::atomic<double> m_speed{1.0};


    std::atomic<double> m_frameDurationMs{1000.0 / 60.0};

    std::atomic<bool> m_isPause = false;

    std::atomic<bool> m_isExit = false;

    // 渲染线程本体；m_running 由 run() 首尾维护，语义同原来的 QThread::isRunning()
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    //待执行的逐帧请求次数
    std::atomic<int> m_stepReq{0};

    std::atomic<long long> m_lastFramePts = 0;
    bool m_firstFrame = false;

    // QElapsedTimer loseTimer;
};

#endif // VIDEORENDERTHREAD_H
