#include "decodethread.h"
#include <QDebug>
extern "C"
{
#include<libavcodec/avcodec.h>

}

DecodeThread::DecodeThread(QObject *parent)
    : QThread{parent}
{

}

DecodeThread::~DecodeThread()
{
    setExit(true); // 析构时才设为 true
    // quit();
    wait();
    close();
}

void DecodeThread::push(AVPacket *pkt)
{
    
    if(!pkt){
        return;
    }
    while(!m_isExit){
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if(m_pktQue.size()<m_maxSize){
                m_pktQue.push(pkt);
                // qDebug()<<m_pktQue.size();
                return;
            }
        }
        msleep(1);
    }
    av_packet_free(&pkt);

}

AVPacket *DecodeThread::pop()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if(!m_pktQue.empty()){
        AVPacket* pkt = m_pktQue.front();
        m_pktQue.pop();
        // qDebug()<<"m_pktQue.pop";
        return pkt;
    }
    return nullptr;
}

void DecodeThread::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_pktQue.empty()) {
        AVPacket *pkt = m_pktQue.front();
        m_pktQue.pop();
        av_packet_free(&pkt);
    }
}

void DecodeThread::close()
{
    // setExit(true);
    // quit();
    // wait();

    clear();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_codec_ctx) {
        avcodec_free_context(&m_codec_ctx);
        m_codec_ctx = nullptr;
    }
    m_codec = nullptr;
}

void DecodeThread::setExit(bool isExit)
{
    m_isExit = isExit;
}

void DecodeThread::setMaxSize(int size)
{
    m_maxSize = size;
}

bool DecodeThread::send(AVPacket *pkt)
{
    // qDebug()<<"bool DecodeThread::send";
    //容错处理
    if (!pkt || pkt->size <= 0 || !pkt->data){
        return false;
    }
    int ret ;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(!m_codec_ctx){
            qDebug()<<"!m_codec_ctx";
            return false;
        }
        ret= avcodec_send_packet(m_codec_ctx,pkt);
    }

    av_packet_free(&pkt);
    return ret==0;
}

AVFrame *DecodeThread::recv()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_codec_ctx)
    {
        return nullptr;
    }
    AVFrame *frame = av_frame_alloc();
    int ret = avcodec_receive_frame(m_codec_ctx,frame);
    if (ret != 0)
    {
        av_frame_free(&frame);
        return nullptr;
    }
    pts = frame->pts;
    return frame;
}




bool DecodeThread::codecInit(AVCodecParameters *para)
{
    //锁内调用
    //找到音频解码器
    m_codec = avcodec_find_decoder(para->codec_id);
    if (!m_codec) {
        return false;
    }
    //创建解码器上下文
    m_codec_ctx = avcodec_alloc_context3(m_codec);
    //配置解码器上下文参数
    avcodec_parameters_to_context(m_codec_ctx,para);
    //设个八线程解码
    m_codec_ctx->thread_count = 8;
    //打开解码器上下文
    int ret = avcodec_open2(m_codec_ctx, NULL, NULL);
    if (ret != 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        qDebug()<< "avcodec_open2 failed!:" << err_buf;
        return false;
    }
    return true;
}

void DecodeThread::flushBuf()
{
    avcodec_flush_buffers(m_codec_ctx);
}
