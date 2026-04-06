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
    long long synpts = 0;

    explicit VideoThread();
    ~VideoThread();
    //
    bool open(AVCodecParameters *para,VideoWidget* widget,int width,int height);



    //给seek做的函数，如果没到达指定pos就释放，到了就显示并释放
    bool repaintPts(AVPacket *pkt, long long seekpts);

    //暂停
    void setPause(bool isPause);

    void run();
signals:
private:
    bool m_isPause = false;
    std::mutex m_viMutex;
    VideoWidget* m_widget;
};

#endif // VIDEOTHREAD_H
