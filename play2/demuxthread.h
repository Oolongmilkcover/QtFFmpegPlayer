#pragma once
#include <QThread>
#include "videocall.h"
#include <mutex>
class Demux;
class VideoThread;
class AudioThread;
class DemuxThread:public QThread
{
public:
    //创建对象并打开
    virtual bool Open(const char *url, VideoCall *call);

    //启动所有线程
    virtual void Start();

    //关闭线程清理资源
    virtual void Close();
    virtual void Clear();

    virtual void Seek(double pos);

    void run();
    DemuxThread();
    virtual ~DemuxThread();
    bool isExit = false;
    long long pts = 0;
    long long totalMs = 0;
    void SetPause(bool isPause);
    bool isPause = false;
protected:
    std::mutex mux;
    Demux *demux = 0;
    VideoThread *vt = 0;
    AudioThread *at = 0;
};

