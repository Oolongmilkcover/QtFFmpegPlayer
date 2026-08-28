#include "videodecodethread.h"
#include "videorenderthread.h"
#include"videowidget.h"
extern"C"{
#include "libavutil/frame.h"
#include "libavcodec/packet.h"
#include "libavcodec/avcodec.h"
}

VideoDecodeThread::VideoDecodeThread(int frameSize, bool keep_last )
{
    m_frameQue = new FrameQueue(frameSize,keep_last);
    m_pktQue = new PacketQueue();
    m_videoRenderThread = new VideoRenderThread(m_frameQue);

}

VideoDecodeThread::~VideoDecodeThread()
{
    close();
    delete m_videoRenderThread;
    m_videoRenderThread = nullptr;
    delete m_pktQue;
    m_pktQue  = nullptr;
    delete m_frameQue;
    m_frameQue = nullptr;
}

bool VideoDecodeThread::open(VideoWidget *widget, int width, int height,AVStream* videoStream)
{
    qDebug()<< "VideoDecodeThread::open!";
    if (!videoStream||!widget)return false;
    close();  //停止上一轮残留线程
    m_isExit = false;
    if (m_pktQue)   m_pktQue->reset();
    if (m_frameQue) m_frameQue->reset();
    m_videoStream = videoStream;
    AVCodecParameters *para = videoStream->codecpar;

    m_widget = widget;
    m_widget->Init(width,height);
    m_serial = 0;

    m_videoRenderThread->serial = 0;

    bool ret = codecInit(para);
    if (ret) {
        setFps(videoStream);
        m_videoRenderThread->setWidget(m_widget);
        m_videoRenderThread->restart();
    }
    qDebug()<< "video Decode open !";
    qDebug()<< "video Render open !";
    return ret;
}

bool VideoDecodeThread::repaintPts(AVPacket *pkt, int64_t seekpts,int serial)
{
    if (!pkt) return false;
    m_serial = serial;
    m_videoRenderThread->serial = serial;
    std::lock_guard<std::mutex> lock(m_viMutex);
    if (!m_codec_ctx) {
        av_packet_free(&pkt);
        return false;
    }
    // 保存原 skip_frame 设置
    auto original_skip = m_codec_ctx->skip_frame;
    // 设置为只解码参考帧（I帧和P帧），跳过B帧等非参考帧
    m_codec_ctx->skip_frame = AVDISCARD_NONREF;
    // 发送 pkt（send 内部会释放 pkt）
    if (!send(pkt)) {
        m_codec_ctx->skip_frame = original_skip;  // 恢复
        return false;
    }

    AVFrame *frame = nullptr;
    bool found = false;

    while (!found && !m_isExit) {
        frame = recv();
        if (!frame) break;
        int64_t raw = (frame->pts != AV_NOPTS_VALUE) ? frame->pts :
                          frame->best_effort_timestamp;
        int64_t frameMs = (raw == AV_NOPTS_VALUE) ? 0
                        :av_rescale_q(raw, m_videoStream->time_base, {1, 1000});
        if (frameMs >= seekpts) {
            // 找到目标帧：恢复完整解码模式直接显示
            m_codec_ctx->skip_frame = original_skip;
            paint(frame);
            m_videoRenderThread->pts.store(seekpts);
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

void VideoDecodeThread::setPause(bool isPause)
{
    m_isPause = isPause;
    m_videoRenderThread->setPause(isPause);
}

void VideoDecodeThread::setRenderPause(bool isPause)
{
    m_videoRenderThread->setPause(isPause);
}

void VideoDecodeThread::paint(AVFrame* frame)
{
    if(!m_widget){
        av_frame_free(&frame);
        return;
    }
    QMetaObject::invokeMethod(
        m_widget,
        [this, frame]() {
            m_widget->setPaint(frame);
        },
        Qt::QueuedConnection
        );
}

void VideoDecodeThread::run()
{
    qDebug() << "VideoDecodeThread running...";
    while(!m_isExit){
        if (m_isPause.load())
        {
            msleep(5);
            continue;
        }
        // 消费packet 生产frame
        // PacketQueue → Decoder → FrameQueue
        //从 PacketQueue 取出一个包 (阻塞)
        auto packet = m_pktQue->pop();
        if (!packet) {
            if (m_isExit) break;
            msleep(1);
            continue;
        }
        int pktSerial = packet->m_serial;
        if(packet->m_serial != m_serial.load()){
            continue;//智能指针自动释放
        }
        //开始解码
        if(!send(packet)){
            qDebug() << "!send(packet)";
            continue;
        }
        //一包多帧，recv到FrameQueue
        while(!m_isExit){
            //得到一个解码后的帧
            AVFrame* avFrame = recv();
            if(!avFrame){
                break;
            }
            int64_t raw = avFrame->best_effort_timestamp;
            if (raw == AV_NOPTS_VALUE) raw = avFrame->pts;
            if (raw == AV_NOPTS_VALUE) raw = 0;
            int64_t tmpPts = av_rescale_q(raw, m_videoStream->time_base, {1, 1000});
            //获取帧队列的可写帧
            Frame* frame = m_frameQue->getWritable();
            if (!frame) {
                av_frame_free(&avFrame);
                break;
            }
            av_frame_unref(frame->m_frame);
            //将本pkt的serial写入frame
            frame->m_serial = pktSerial;
            //转移avFrame的buffer
            av_frame_move_ref(frame->m_frame,avFrame);
            av_frame_free(&avFrame);
            frame->m_frame->pts = tmpPts;
            //告诉帧队列填充完毕
            m_frameQue->push();
        }
        msleep(1);
    }
    qDebug() << "VideoDecodeThread end...";
}

void VideoDecodeThread::setLastSome(bool lastSome)
{
    m_videoRenderThread->lastSome = lastSome;
}

long long VideoDecodeThread::getVideoRenderPts()
{
    return m_videoRenderThread->pts.load();
}

void VideoDecodeThread::setFps(AVStream* videoStream)
{
    m_videoRenderThread->setFps(av_q2d(videoStream->avg_frame_rate));  // 或从 AVStream 获取
    //qDebug()<<"av_q2d(videoStream->avg_frame_rate)"<<av_q2d(videoStream->avg_frame_rate);
}

void VideoDecodeThread::close()
{
    // 先停渲染线程并 join —— 它是 FrameQueue 的消费者
    if (m_videoRenderThread)
        m_videoRenderThread->stop();
    //再停解码线程并 join
    m_isExit = true;
    if (m_pktQue)   m_pktQue->abort();
    if (m_frameQue) m_frameQue->abort();
    wait();
    // 两个线程都退出了，清队列才安全
    DecodeThread::clear();
    // 释放解码器
    DecodeThread::close();
}

void VideoDecodeThread::stopAndClear()
{
    close();                          // 复用上面的完整停机
    if (m_pktQue)   m_pktQue->reset();
    if (m_frameQue) m_frameQue->reset();
    m_isExit = false;
}

void VideoDecodeThread::restart()
{
    if (m_pktQue)   m_pktQue->reset();
    if (m_frameQue) m_frameQue->reset();
    m_isExit = false;
    if (m_videoRenderThread)
        m_videoRenderThread->restart();
    start();
}

void VideoDecodeThread::setStepFrameMode(int mode)
{
    m_videoRenderThread->m_FrameStepMode.store(mode);
}

void VideoDecodeThread::setSerial(int serial)
{
    m_serial.store(serial);
    m_videoRenderThread->serial.store(serial) ;
}

bool VideoDecodeThread::getPlayDone()
{
    bool playDone = m_videoRenderThread->playDone.load();
    if(playDone){
        m_videoRenderThread->playDone.store(false);
        return true;
    }
    return false;
}

void VideoDecodeThread::setSynpts(long long synpts)
{
    m_videoRenderThread->synpts.store(synpts);
}
