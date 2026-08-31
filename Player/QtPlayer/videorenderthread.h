#ifndef VIDEORENDERTHREAD_H
#define VIDEORENDERTHREAD_H

#include <QThread>
#include <atomic>
#include <QElapsedTimer>
#include "framequeue.h"

class VideoWidget;
class VideoRenderThread : public QThread
{
    Q_OBJECT
private:
    QElapsedTimer m_loopTimer;
    bool firstFrame = true;
public:

    explicit VideoRenderThread(FrameQueue* frameQueue);
    ~VideoRenderThread() override;
    //设置VideoWidget
    void setWidget(VideoWidget* widget);
    //设置视频帧率
    void setFps(double fps);

    // 设置倍速
    void setSpeed(double speed);
    /*
     * 外部音频时钟
     *
     * AudioThread负责不断更新它
     *
     * VideoRenderThread只读取
     */
    std::atomic<long long> synpts = 0;
    //音频是否已经结束
    std::atomic<bool> lastSome = false;
    std::atomic<int> serial = 0;
    std::atomic<long long> pts = 0;
    void setPause(bool isPause);
    void setSerial(int serial);
    void stop();

    void restart();

    std::atomic<bool> playDone = false;

    //0无 1下一帧  2上一帧
    std::atomic<int> m_FrameStepMode = 0;

protected:

    void run() override;

private:

    void sleepUntil(qint64 targetWallMs);

    void renderFrame(Frame* frame);

private:

    VideoWidget* m_widget = nullptr;

    FrameQueue* m_frameQueue = nullptr;

    double m_fps = 60;

    // 倍速（视频帧时长 = 1000/fps/speed）
    std::atomic<double> m_speed{1.0};


    double m_frameDurationMs = 1000.0 / m_fps;

    std::atomic<bool> m_isPause = false;

    std::atomic<bool> m_isExit = false;

    std::atomic<bool> m_isPlayPrevFrame = false;
};

#endif // VIDEORENDERTHREAD_H
