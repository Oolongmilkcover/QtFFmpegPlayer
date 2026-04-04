#include "videothread.h"
#include "decode.h"
#include <iostream>
#include <QElapsedTimer>
using namespace std;
//打开，不管成功与否都清理
bool VideoThread::Open(AVCodecParameters *para, VideoCall *call,int width,int height,double fps)
{
    if (!para)return false;
    Clear();

    vmux.lock();
    synpts = 0;
    //初始化显示窗口
    this->call = call;
    synpts = 0;
    this->call = call;
    this->fps = (fps > 0 && fps < 120) ? fps : 25.0;

    if (call)
    {
        call->Init(width, height);
    }
    vmux.unlock();
    int re = true;
    if (!decode->Open(para))
    {
        cout << "audio Decode open failed!" << endl;
        re = false;
    }

    cout << "XAudioThread::Open :" << re << endl;
    return re;
}
void VideoThread::SetPause(bool isPause)
{
    vmux.lock();
    this->isPause = isPause;
    vmux.unlock();
}
void VideoThread::run()
{
    QElapsedTimer timer; //60帧计时器
    timer.start();
    const qint64 frameInterval = 1000 / 60;

    while (!isExit)
    {
        vmux.lock();
        if (this->isPause)
        {
            vmux.unlock();
            msleep(5);
            timer.restart();
            continue;
        }
        //动态计算帧间隔：1000ms / 原视频帧率
        const qint64 frameInterval = static_cast<qint64>(1000.0 / fps);

        //音视频同步
        if (synpts > 0 && decode->pts > synpts + 40)// 允许40ms误差
        {
            vmux.unlock();
            msleep(1);
            continue;
        }
        AVPacket *pkt = Pop();
        bool re = decode->Send(pkt);
        if (!re)
        {
            vmux.unlock();
            msleep(1);
            continue;
        }
        //一次send 多次recv
        while (!isExit)
        {
            AVFrame * frame = decode->Recv();
            if (!frame)break;

            //精确控制帧率，匹配原视频速度
            qint64 elapsed = timer.elapsed();
            if (elapsed < frameInterval)
            {
                vmux.unlock();
                msleep(frameInterval - elapsed);
                vmux.lock();
            }
            timer.restart();

            //显示视频
            if (call)
            {
                call->Repaint(frame);
            }

        }
        vmux.unlock();
    }
}
//解码pts，如果接收到的解码数据pts >= seekpts return true 并且显示画面
bool VideoThread::RepaintPts(AVPacket *pkt, long long seekpts)
{
    vmux.lock();
    bool re = decode->Send(pkt);
    if (!re)
    {
        vmux.unlock();
        return true; //表示结束解码
    }
    AVFrame *frame = decode->Recv();
    if (!frame)
    {
        vmux.unlock();
        return false;
    }
    //到达位置
    if (decode->pts >= seekpts)
    {
        if(call)
            call->Repaint(frame);
        vmux.unlock();
        return true;
    }
    FreeFrame(&frame);
    vmux.unlock();
    return false;
}
VideoThread::VideoThread()
{
}


VideoThread::~VideoThread()
{

}
