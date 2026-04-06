#include "videothread.h"
#include"videowidget.h"
extern"C"{
#include "libavutil/frame.h"
#include <libavcodec/packet.h>
}


VideoThread::VideoThread()
{

}

VideoThread::~VideoThread()
{
    close();
}

bool VideoThread::open(AVCodecParameters *para, VideoWidget *widget, int width, int height)
{
    if (!para||!widget)return false;
    clear();

    m_viMutex.lock();
    synpts = 0;
    //初始化显示窗口
    m_widget = widget;
    m_widget->init(width,height);
    m_viMutex.unlock();

    bool ret = true;
    //视频解码器初始化
    if(!codecInit(para)){
        qDebug()<< "audio Decode open failed!";
        ret = false;
    }
    return ret;
}

bool VideoThread::repaintPts(AVPacket *pkt, long long seekpts)
{
    std::lock_guard<std::mutex> lock(m_viMutex);
    bool ret = send(pkt);
    if(!ret){
        return true;
    }
    AVFrame* frame=  recv();
    if (!frame)
    {
        return false;
    }
    //到达位置
    if (pts >= seekpts)
    {
        if(m_widget)
            m_widget->repaint(frame);
        return true;
    }
    av_frame_free(&frame);
    return false;
}

void VideoThread::setPause(bool isPause)
{
    m_viMutex.lock();
    m_isPause = isPause;
    m_viMutex.unlock();
}

void VideoThread::run()
{
    while(!m_isExit){
        // 暂停就阻塞
        {
            std::lock_guard<std::mutex> lock(m_viMutex);
            if (m_isPause) {
                msleep(5);
                continue;
            }
        }
        //取出一个pkt
        AVPacket *pkt = pop();
        if (!pkt) {
            msleep(1);
            continue;
        }
        //send失败跳过下一个包
        if(!send(pkt)){
            continue;
        }

        while(!m_isExit){
            AVFrame* frame = recv();
            //这个包显示完了，下个包
            if(!frame){
                break;
            }
            //同步:如果视频超前音频，等待
            if (synpts > 0 && pts > synpts + 40) {
                int wait = pts - synpts;
                msleep(wait);
                //睡眠后只打印这个包的这一帧
                if (m_widget){
                    m_widget->repaint(frame);
                }
                break;
            }
            if (m_widget){
                m_widget->repaint(frame);
            }else{
                av_frame_free(&frame);
            }
        }
    }
}
