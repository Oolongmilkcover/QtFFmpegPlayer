/*
解码视频
同步音频时钟
按原视频帧率显示
传给 VideoWidget
*/

#ifndef VIDEODECODETHREAD_H
#define VIDEODECODETHREAD_H

#include "decodethread.h"



#include <QObject>
class VideoWidget;
class VideoRenderThread;
class VideoDecodeThread : public DecodeThread
{
    Q_OBJECT
public:
    //同步时间，由外部传入
    void setSynpts(long long synpts);

    explicit VideoDecodeThread(int frameSize = 100 , bool keep_last = true);
    ~VideoDecodeThread();
    //
    bool open(VideoWidget* widget,int width,int height,AVStream* videoStream);

    //给seek做的函数，如果没到达指定pos就释放，到了就显示并释放
    bool repaintPts(AVPacket *pkt, int64_t seekpts,int serial);

    //暂停
    void setPause(bool isPause);
    //渲染暂停
    void setRenderPause(bool isPause);

    //paint
    void paint(AVFrame* frame);

    void run() override;

    //设置“还有视频未播放”
    void setLastSome(bool lastSome);

    long long getVideoRenderPts();

    //设置fps
    void setFps(AVStream* videoStream);

    // 完全停止并释放解码器
    void close();

    // 只停线程+清队列
    void stopAndClear();

    // 重启解码线程
    void restart();

    //设置逐帧模式
    void setStepFrameMode(int mode);

    void setSerial(int serial);

    bool getPlayDone();

private:
    std::atomic<bool> m_isPause = false;
    VideoWidget* m_widget;
    //视频渲染线程  framequeue消费者
    VideoRenderThread *m_videoRenderThread = nullptr;
    AVStream* m_videoStream = nullptr;
    std::mutex m_viMutex;
};

#endif // VIDEODECODETHREAD_H
