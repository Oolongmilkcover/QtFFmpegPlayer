#include "demuxthread.h"
#include "demux.h"
#include "videothread.h"
#include "audiothread.h"
#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
}

using namespace std;
void DemuxThread::Clear()
{
    mux.lock();
    if (demux) demux->Clear();
    if (vt) vt->Clear();
    if (at) at->Clear();
    mux.unlock();
}

void DemuxThread::Seek(double pos)
{
    //1：Seek前先暂停，但不要在锁里暂停
    bool status = this->isPause;
    SetPause(true);

    // 清理缓存
    Clear();

    mux.lock();
    if (!demux)
    {
        mux.unlock();
        if (!status) SetPause(false);
        return;
    }

    // 执行Seek
    demux->Seek(pos);
    long long seekPts = pos * demux->totalMs;
    mux.unlock();

    //2：Seek后的解码显示不在锁里，避免界面卡死
    while (!isExit)
    {
        AVPacket *pkt = demux->ReadVideo();
        if (!pkt) break;

        if (vt->RepaintPts(pkt, seekPts))
        {
            this->pts = seekPts;
            break;
        }
    }

    // 恢复播放状态
    if (!status) SetPause(false);
}

void DemuxThread::SetPause(bool isPause)
{
    mux.lock();
    this->isPause = isPause;
    if (at) at->SetPause(isPause);
    if (vt) vt->SetPause(isPause);
    mux.unlock();
}

void DemuxThread::run()
{
    while (!isExit)
    {
        mux.lock();
        if (isPause)
        {
            mux.unlock();
            msleep(5);
            continue;
        }
        if (!demux)
        {
            mux.unlock();
            msleep(5);
            continue;
        }

        // 音视频同步
        if (vt && at)
        {
            pts = at->pts;
            vt->synpts = at->pts;
        }

        AVPacket *pkt = demux->Read();
        if (!pkt)
        {
            mux.unlock();
            msleep(5);
            continue;
        }

        // 判断数据是音频
        if (demux->IsAudio(pkt))
        {
            if (at) at->Push(pkt);
        }
        else // 视频
        {
            if (vt) vt->Push(pkt);
        }
        mux.unlock();
        msleep(1);
    }
}

bool DemuxThread::Open(const char *url, VideoCall *call)
{
    if (url == 0 || url[0] == '\0')
        return false;

    mux.lock();
    if (!demux) demux = new Demux();
    if (!vt) vt = new VideoThread();
    if (!at) at = new AudioThread();

    // 打开解封装
    bool re = demux->Open(url);
    if (!re)
    {
        mux.unlock();
        cout << "demux->Open(url) failed!" << endl;
        return false;
    }

    //获取原视频帧率
    double videoFps = 25.0;
    if (demux->ic && demux->videoStream >= 0)
    {
        videoFps = av_q2d(demux->ic->streams[demux->videoStream]->avg_frame_rate);
        if (videoFps <= 0 || videoFps > 120)
        {
            videoFps = av_q2d(demux->ic->streams[demux->videoStream]->r_frame_rate);
        }
    }

    // 打开视频解码器和处理线程
    if (!vt->Open(demux->CopyVPara(), call, demux->width, demux->height,videoFps))
    {
        re = false;
        cout << "vt->Open failed!" << endl;
    }
    // 打开音频解码器和处理线程
    if (!at->Open(demux->CopyAPara(), demux->sampleRate, demux->channels))
    {
        re = false;
        cout << "at->Open failed!" << endl;
    }
    totalMs = demux->totalMs;
    mux.unlock();

    cout << "XDemuxThread::Open " << re << endl;
    return re;
}

void DemuxThread::Close()
{
    isExit = true;
    wait();
    if (vt) vt->Close();
    if (at) at->Close();
    mux.lock();
    delete vt;
    delete at;
    vt = NULL;
    at = NULL;
    mux.unlock();
}

void DemuxThread::Start()
{
    mux.lock();
    if (!demux) demux = new Demux();
    if (!vt) vt = new VideoThread();
    if (!at) at = new AudioThread();
    QThread::start();
    if (vt) vt->start();
    if (at) at->start();
    mux.unlock();
}

DemuxThread::DemuxThread()
{
}

DemuxThread::~DemuxThread()
{
    isExit = true;
    wait();
}
