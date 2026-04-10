/*
解码视频
同步音频时钟
按原视频帧率显示
传给 VideoWidget
*/

#ifndef VIDEOTHREAD_H
#define VIDEOTHREAD_H

#include "decodethread.h"



#include <QObject>
class VideoWidget;
class VideoThread : public DecodeThread
{
    Q_OBJECT
public:
    //同步时间，由外部传入
    std::atomic<long long> synpts = 0;

    explicit VideoThread();
    ~VideoThread();
    //
    bool open(AVCodecParameters *para,VideoWidget* widget,int width,int height);



    //给seek做的函数，如果没到达指定pos就释放，到了就显示并释放
    bool repaintPts(AVPacket *pkt, int64_t seekpts);

    //暂停
    void setPause(bool isPause);

    //paint
    void paint(AVFrame* frame);

    void run() override;

    std::atomic<bool> lastSome = false;
signals:
    void setDone();
private:
    bool m_isPause = false;
    std::mutex m_viMutex;
    VideoWidget* m_widget;
    double m_fps = 60;  // 或从 AVStream 获取
    double m_frame_duration_ms = 1000.0 / m_fps;      // 每帧应显示的时间（毫秒）
};

#endif // VIDEOTHREAD_H
