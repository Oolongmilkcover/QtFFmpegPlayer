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
    //1.创建视频音频线程
    m_videoThread = new VideoThread();
    m_audioThread = new AudioThread();
    connect(m_videoThread,&VideoThread::setDone,this,&DemuxThread::setDone);
}

DemuxThread::~DemuxThread()
{
    m_isExit = true;
    wait();

    // 释放音视频线程
    delete m_videoThread;
    delete m_audioThread;
    m_videoThread = nullptr;
    m_audioThread = nullptr;

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
    m_mutex.lock();
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
    totalMs = sec*1000; // 换算成毫秒
    qDebug()<<"totalMs:" << totalMs ;
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
    m_width = vpara->width;
    m_height = vpara->height;
    if(!m_videoThread->open(vpara,widget,vpara->width,vpara->height)){
        tmpRet = false;
        qDebug()<<"m_videoThread->open failed";
    }
    // // 打开音频解码器和处理线程
    AVCodecParameters *apara = m_fmt_ctx->streams[m_audioStream]->codecpar;
    if(!m_audioThread->open(apara,apara->sample_rate,apara->ch_layout.nb_channels)){
        tmpRet = false;
        qDebug()<<"m_audioThread->open failed";
    }
    qDebug()<<"DemuxThread::Open :"<<tmpRet;
    if(!tmpRet){
        m_mutex.unlock();
        closeAVThread();
    }else{
        isCompleteInit = true;
        m_mutex.unlock();
        setPause(false);
        qDebug()<<"openFile end";
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
    m_isPause.store(isPause);
    if (m_audioThread) m_audioThread->setPause(isPause);
    if (m_videoThread) m_videoThread->setPause(isPause);
}

bool DemuxThread::seek(double pos)
{
    if(pos == 0.0 && playDone ){
        return false;
    }
    setPause(false);
    QThread::usleep(500);
    emit disableBtn();
    m_seekPos = pos;
    m_isSeeking = true;
    return true;
}


void DemuxThread::close()
{

    setPause(true);  // 先暂停
    clear();         // 清空队列

    closeAVThread(); // 关闭音视频线程

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fmt_ctx) {
        avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }

    m_videoStream = -1;
    m_audioStream = -1;
    totalMs = 0;
    isCompleteInit = false;
    pts = 0;
}

void DemuxThread::clear()
{
    if (m_videoThread) m_videoThread->clear();
    if (m_audioThread) m_audioThread->clear();
}

void DemuxThread::closeAVThread()
{
    if (m_videoThread) m_videoThread->close();
    if (m_audioThread) m_audioThread->close();
}

AVPacket *DemuxThread::readPkt()
{
    // qDebug()<<"readPkt";
    m_mutex.lock();
    bool valid = m_fmt_ctx != nullptr;
    m_mutex.unlock();

    if(!valid){
        return nullptr;
    }

    //初始化pkt
    AVPacket* pkt = av_packet_alloc();
    //读取一帧，并分配空间
    int re = av_read_frame(m_fmt_ctx, pkt);
    if (re == AVERROR_EOF) {
        m_eof.store(true);
        av_packet_free(&pkt);
        return nullptr;
    }
    if (re != 0)
    {
        av_packet_free(&pkt);
        return nullptr;
    }


    AVRational tb = m_fmt_ctx->streams[pkt->stream_index]->time_base;
    //pts转换为毫秒
    pkt->pts = pkt->pts*(1000 * (av_q2d(tb)));
    pkt->dts = pkt->dts*(1000 * (av_q2d(tb)));
    return pkt;
}

bool DemuxThread::getIsPause()
{
    return m_isPause;
}

long long DemuxThread::getVideoPts()
{
    return m_videoThread->pts.load();
}

void DemuxThread::run()
{
    static int count = 1;
    while(!m_isExit){
        // ===== 1. 先处理 seek  =====
        if (m_isSeeking)
        {
            m_isSeeking = false;

            // 1. 先保存暂停状态
            bool wasPause = m_isPause;
            setPause(true);

            // 2. 清空队列（快速，加锁没问题）
            clear();
            m_mutex.lock();
            if (!isCompleteInit || !m_fmt_ctx || m_videoStream < 0) {
                m_mutex.unlock();
                setPause(wasPause);
                return;
            }
            // 执行seek（瞬间完成）
            avformat_flush(m_fmt_ctx);
            int64_t seekMs = m_seekPos * totalMs;
            //统一格式 转为FFmpeg 内部基
            int64_t ts = av_rescale_q(seekMs,{1, 1000},
                                      m_fmt_ctx->streams[m_videoStream]->time_base);

            av_seek_frame(m_fmt_ctx, m_videoStream, ts, AVSEEK_FLAG_BACKWARD);
            if (m_videoThread)
            {
                m_videoThread->flushBuf();
            }

            if (m_audioThread)
            {
                m_videoThread->flushBuf();
            }
            m_mutex.unlock();
            while (!m_isExit)
            {
                AVPacket *pkt = readPkt(); // 内部自己加锁、快速释放
                if (!pkt) break;

                if (pkt->stream_index == m_videoStream) {
                    // repaintPts 内部只在解码瞬间加锁
                    bool found = m_videoThread->repaintPts(pkt, seekMs);
                    if (found) break;
                } else {
                    av_packet_free(&pkt);
                }
            }

            // 恢复暂停
            setPause(wasPause);
            emit ableBtn();
            continue;  //非常重要：本轮结束
        }

        // 暂停或未初始化 → 等待
        if(m_isPause || !isCompleteInit)
        {
            msleep(5);
            continue;

        }
        // 音视频同步
        bool tmp = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_videoThread && m_audioThread)
            {
                tmp = true;
            }
        }
        if(tmp){
            pts.store(m_audioThread->pts);
            // emit moveSlider(pts.load());
            // pts.store(m_videoThread->synpts.load());
            m_videoThread->synpts.store(m_audioThread->pts);
        }
        AVPacket *pkt = readPkt();
        if (!pkt)
        {
            // if(pts>0&&totalMs>0&&pts>=totalMs*0.8){
            if(totalMs>0&&m_eof){
                m_videoThread->lastSome.store(true);
                playDone.store(true);
                qDebug()<<"!pkt";
                int timeCount = 0;
                while(!seek(0.0)){
                    msleep(10);
                    timeCount++;
                    if(timeCount>=200){
                        playDone.store(false);
                    }
                    // qDebug()<<"seek0.0 fail" ;
                }
                m_eof.store(false);
                continue;
            }
            msleep(5);
            continue;
        }else{
            m_videoThread->lastSome.store(false);
        }
        // 判断数据是音频
        if(pkt->stream_index == m_videoStream && m_videoThread){
            //视频
            // qDebug()<<"m_videoThread->push(pkt):"<<count++;
            m_videoThread->push(pkt);

        }else if(pkt->stream_index == m_audioStream && m_audioThread){
            //音频
            // qDebug()<<"m_audioThread->push(pkt)";
            m_audioThread->push(pkt);

        }else{
            av_packet_free(&pkt);
        }
        // msleep(1);
    }
}

void DemuxThread::setDone()
{

    // qDebug()<<"get emit";
    playDone.store(false);

}
