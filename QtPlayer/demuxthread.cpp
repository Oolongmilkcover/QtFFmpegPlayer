#include "demuxthread.h"
#include "audiothread.h"
#include "videothread.h"
#include "videowidget.h"
#include <QDebug>

extern "C" {
#include <libavformat/avformat.h>
}
DemuxThread::DemuxThread(QObject *parent)
    : QThread{parent}
{
    //1.网络流初始化
    avformat_network_init();
    //设置rtsp流以tcp协议打开
    av_dict_set(&m_option, "rtsp_transport", "tcp", 0);
    //网络延时时间
    av_dict_set(&m_option, "max_delay", "500", 0);
}

DemuxThread::~DemuxThread()
{
    m_isExit = true;
    wait();

    // 释放全局配置字典
    if (m_option)
    {
        av_dict_free(&m_option);
        m_option = nullptr;
    }
}

bool DemuxThread::openFile(const char* url,VideoWidget* widget)
{
    if (url == 0 || url[0] == '\0'){
        return false;
    }
    close();
    bool tmpRet = true;
    std::lock_guard<std::mutex> lock(m_mutex);
    //1.创建视频音频线程
    if (!m_videoThread) m_videoThread = new VideoThread();
    if (!m_audioThread) m_audioThread = new AudioThread();
    //2.打开解封装打开输入流
    int ret = avformat_open_input(&m_fmt_ctx, url, NULL, &m_option);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        qDebug()<< "open" << url << "failed!:" << err_buf;
        // 释放已创建的线程
        closeAVThread();
        return false;
    }
    //3.读取流信息
    ret = avformat_find_stream_info(m_fmt_ctx, NULL);
    if (ret < 0) {
        qDebug()<< "读取流信息失败";
        avformat_close_input(&m_fmt_ctx);
        // 释放已创建的线程
        closeAVThread();
        return false;
    }
    //获取时长
    double sec = (double)m_fmt_ctx->duration / AV_TIME_BASE; //秒
    m_totalMs = sec*1000; // 换算成毫秒
    qDebug()<<"totalMs:" << m_totalMs ;
    //打印视频流详细信息
    av_dump_format(m_fmt_ctx, 0, url, 0);

    //获取音视频流信息
    m_videoStream = av_find_best_stream(m_fmt_ctx,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
    if (m_videoStream < 0) {
        // 释放已创建的线程
        closeAVThread();
        return false;
    }
    m_audioStream = av_find_best_stream(m_fmt_ctx,AVMEDIA_TYPE_AUDIO,-1,-1,NULL,0);
    if (m_audioStream < 0) {
        // 释放已创建的线程
        closeAVThread();
        return false;
    }

    // 打开视频解码器和处理线程
    AVCodecParameters *vpara = m_fmt_ctx->streams[m_videoStream]->codecpar;
    if(!m_videoThread->open(vpara,widget,vpara->width,vpara->height)){
        tmpRet = false;
        qDebug()<<"m_videoThread->open failed";
    }
    // 打开视频解码器和处理线程
    AVCodecParameters *apara = m_fmt_ctx->streams[m_audioStream]->codecpar;
    if(!m_audioThread->open(apara,apara->sample_rate,apara->ch_layout.nb_channels)){
        tmpRet = false;
        qDebug()<<"m_audioThread->open failed";
    }
    qDebug()<<"DemuxThread::Open :"<<tmpRet;
    if(!tmpRet){
        closeAVThread();
    }else{
        isCompleteInit = true;
    }
    return tmpRet;
}

void DemuxThread::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QThread::start();
    if (m_videoThread) m_videoThread->start();
    if (m_audioThread) m_audioThread->start();

}

void DemuxThread::setPause(bool isPause)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isPause = isPause;
    if (m_audioThread) m_audioThread->setPause(isPause);
    if (m_videoThread) m_videoThread->setPause(isPause);
}

void DemuxThread::close()
{
    m_isExit = true;
    wait();
    if (m_videoThread) m_videoThread->close();
    if (m_audioThread) m_audioThread->close();
    std::lock_guard<std::mutex> lock(m_mutex);
    //自己的close
    if (m_fmt_ctx) {
        avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }
    m_videoStream = -1;
    m_audioStream = -1;
    m_totalMs = 0;
    isCompleteInit = false; // 每次重新打开都重置未初始化
    pts = 0;

}

void DemuxThread::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_videoThread) m_videoThread->clear();
    if (m_audioThread) m_audioThread->clear();
    //自己的clear
}

void DemuxThread::closeAVThread()
{
    if (m_videoThread) m_videoThread->close();
    if (m_audioThread) m_audioThread->close();
}

AVPacket *DemuxThread::readPkt()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!m_fmt_ctx){
        return nullptr;
    }
    //初始化pkt
    AVPacket* pkt = av_packet_alloc();
    //读取一帧，并分配空间
    int re = av_read_frame(m_fmt_ctx, pkt);
    if (re != 0)
    {
        av_packet_free(&pkt);
        return nullptr;
    }
    //pts转换为毫秒
    pkt->pts = pkt->pts*(1000 * (av_q2d(m_fmt_ctx->streams[pkt->stream_index]->time_base)));
    pkt->dts = pkt->dts*(1000 * (av_q2d(m_fmt_ctx->streams[pkt->stream_index]->time_base)));

    return pkt;
}

void DemuxThread::run()
{
    while(!m_isExit){
        // 暂停或未初始化 → 等待
        bool is_pause = false;
        bool is_init = false;

        //读取线程共享变量
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            is_pause = m_isPause;
            is_init = isCompleteInit;
        }
        if(is_pause || !is_init)
        {
            msleep(5);
            continue;
        }
        // 音视频同步
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_videoThread && m_audioThread)
            {
                pts = m_audioThread->pts;
                m_videoThread->synpts =m_audioThread->pts;
            }
        }
        AVPacket *pkt = readPkt();
        if (!pkt)
        {
            msleep(5);
            continue;
        }
        // 判断数据是音频
        if(pkt->stream_index == m_videoStream && m_videoThread){
            //视频
            m_videoThread->push(pkt);
        }else if(pkt->stream_index == m_audioStream && m_audioThread){
            //音频
            m_audioThread->push(pkt);
        }else{
            av_packet_free(&pkt);
        }
        // msleep(1);
    }
}
