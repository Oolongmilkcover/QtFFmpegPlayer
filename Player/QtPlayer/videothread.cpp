#include "videothread.h"

#include"videowidget.h"
extern"C"{
#include "libavutil/frame.h"
#include <libavcodec/packet.h>
#include "libavcodec/avcodec.h"
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
    qDebug()<< "VideoThread::open!";
    if (!para||!widget)return false;
    clear();

    // ========== 先初始化（不加锁） ==========
    m_widget = widget;
    m_widget->init(width,height);

    // ========== 再加锁做其他事 ==========
    synpts = 0;

    bool ret = true;
    if(!codecInit(para)){
        qDebug()<< "video Decode open failed!";
        ret = false;
    }
    m_fps = av_q2d(m_codec_ctx->framerate);  // 或从 AVStream 获取
    qDebug()<< "video Decode open !";
    return ret;
}

bool VideoThread::repaintPts(AVPacket *pkt, int64_t seekpts)
{
    std::lock_guard<std::mutex> lock(m_viMutex);

    // 保存原 skip_frame 设置
    auto original_skip = m_codec_ctx->skip_frame;
    // 设置为只解码参考帧（I帧和P帧），跳过B帧等非参考帧
    m_codec_ctx->skip_frame = AVDISCARD_NONREF;

    // 发送 pkt（send 内部会释放 pkt）
    if (!send(pkt)) {
        m_codec_ctx->skip_frame = original_skip;  // 恢复
        return true;
    }

    AVFrame *frame = nullptr;
    bool found = false;

    while (!found && !m_isExit) {
        frame = recv();
        if (!frame) break;
        if (frame->pts >= seekpts) {
            // 找到目标帧：恢复完整解码模式再显示
            m_codec_ctx->skip_frame = original_skip;
            paint(frame);
            found = true;
        } else {
            av_frame_free(&frame);
        }
    }

    // 恢复原设置
    if (!found) {
        m_codec_ctx->skip_frame = original_skip;
    }

    return found;
}


// bool VideoThread::repaintPts(AVPacket *pkt, long long seekpts)
// {
//     std::lock_guard<std::mutex> lock(m_viMutex);

//     if (!send(pkt))
//         return true;

//     AVFrame *frame = recv();
//     if (!frame)
//         return false;

//     bool found = false;
//     if (pts >= seekpts)
//     {
//         paint(frame);
//         found = true;
//     }
//     else
//     {
//         av_frame_free(&frame);
//     }

//     return found;
// }

void VideoThread::setPause(bool isPause)
{
    m_viMutex.lock();
    m_isPause = isPause;
    m_viMutex.unlock();
}

void VideoThread::paint(AVFrame* frame)
{
    if(!m_widget){
        av_frame_free(&frame);
        return;
    }
    QMetaObject::invokeMethod(
        m_widget,
        [this, frame]() {
            m_widget->setpaint(frame);
        },
        Qt::QueuedConnection
        );
}

void VideoThread::run()
{
    qDebug() << "VideoThread running...";
    while(!m_isExit){

        // 暂停就阻塞
        m_viMutex.lock();
        bool isPause = m_isPause;
        m_viMutex.unlock();

        if (isPause) {
            msleep(5);
            continue;
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
        // ========= 一包多帧 =========
        while(!m_isExit){
            AVFrame* frame = recv();
            //这个包显示完了，下个包
            if(!frame){

                break;
            }
            // 同步：如果视频超前音频，等待
            long long diff = pts - synpts;

            if (synpts > 0)
            {
                // qDebug()<<diff;
                //音频先结束了视频得正常播放，此时diff>>100
                if(lastSome.load()){
                    // qDebug()<<"last";
                    msleep((int)m_frame_duration_ms);
                }else
                // ===== 2. 正常同步 =====
                if(diff>40){
                    // int delay = std::min((long long)40, diff);
                    msleep(40);
                }else
                if (diff >= -40&&diff<=40)
                {
                    msleep((int)m_frame_duration_ms);
                }
                else if (diff< -40)
                {
                    // 稍微落后 → 丢一帧
                    av_frame_free(&frame);
                    continue;
                }else if(diff <= -100){
                    av_frame_free(&frame);
                    while(!m_isExit){
                        frame = recv();
                        //这个包丢完了，下个包
                        if(!frame){
                            break;
                        }else{
                            av_frame_free(&frame);
                        }
                    }
                    break;
                }

            }
            // ===== 正常显示 =====

            paint(frame); 
        }
        m_viMutex.lock();
        if(m_pktQue.empty()&&lastSome.load()){
            qDebug()<<"emit";
            // emit setDone();
        }
        m_viMutex.unlock();
    }

    qDebug() << "VideoThread end...";
}
