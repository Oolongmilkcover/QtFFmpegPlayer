#include "demuxthread.h"
#include "audiothread.h"
#include "videodecodethread.h"
#include "videorenderthread.h"
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
    m_videoDecodeThread = new VideoDecodeThread();
    m_audioThread = new AudioThread();

}

DemuxThread::~DemuxThread()
{
    m_isExit = true;
    wait();

    // 释放音视频线程
    delete m_videoDecodeThread;
    delete m_audioThread;
    m_videoDecodeThread = nullptr;
    m_audioThread = nullptr;

    // 释放全局配置字典
    if (m_option)
    {
        av_dict_free(&m_option);
        m_option = nullptr;
    }

    //20260722
    avformat_close_input(&m_fmt_ctx);
}

bool DemuxThread::openFile(const char* url,VideoWidget* widget)
{
    if (url == 0 || url[0] == '\0'){
        return false;
    }
    close();
    m_isExit.store(false);
    m_serial.store(0); //每次加载时serial清零

    bool tmpRet = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        //2.打开解封装打开输入流
        int ret = avformat_open_input(&m_fmt_ctx, url, NULL, &m_option);
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            qDebug()<< "open" << url << "failed!:" << err_buf;
            // 释放已创建的线程
            closeAVThread();
            tmpRet =  false;
        }else{
            //配合rtsp流不卡 退出流程设置的回调函数
            m_fmt_ctx->interrupt_callback.callback = interruptCallback;
            m_fmt_ctx->interrupt_callback.opaque   = this;

            ret = avformat_find_stream_info(m_fmt_ctx, NULL);
            if (ret < 0) {
                qDebug()<< "读取流信息失败";
                avformat_close_input(&m_fmt_ctx);
                // 释放已创建的线程
                closeAVThread();
                tmpRet =  false;
            }
        }
        //获取时长
        double sec = (double)m_fmt_ctx->duration / AV_TIME_BASE; //秒
        totalMs = sec*1000; // 换算成毫秒
        qDebug()<<"totalMs:" << totalMs ;
        //打印视频流详细信息
        av_dump_format(m_fmt_ctx, 0, url, 0);

        if(tmpRet){
            //获取音视频流信息
            m_videoStream = av_find_best_stream(m_fmt_ctx,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
            m_audioStream = av_find_best_stream(m_fmt_ctx,AVMEDIA_TYPE_AUDIO,-1,-1,NULL,0);
            if (m_videoStream < 0 || m_audioStream < 0){
                // 释放已创建的线程
                closeAVThread();
                tmpRet = false;
            }
        }
    }
    if (!tmpRet) {
        if (m_fmt_ctx) {
            avformat_close_input(&m_fmt_ctx);
            m_fmt_ctx = nullptr;
        }
        return false;
    }
    // 打开视频解码器和处理线程
    AVCodecParameters *vpara = m_fmt_ctx->streams[m_videoStream]->codecpar;
    m_width = vpara->width;
    m_height = vpara->height;
    if(!m_videoDecodeThread->open(widget,vpara->width,vpara->height,m_fmt_ctx->streams[m_videoStream])){
        tmpRet = false;
        qDebug()<<"m_videoDecodeThread->open failed";
    }
    // // 打开音频解码器和处理线程
    if(!m_audioThread->open(m_fmt_ctx->streams[m_audioStream])){
        tmpRet = false;
        qDebug()<<"m_audioThread->open failed";
    }
    qDebug()<<"DemuxThread::Open :"<<tmpRet;
    if(!tmpRet){
        closeAVThread();
    }else{
        isCompleteInit = true;
        setPause(false);
        qDebug()<<"openFile end";
    }
    m_audioTimebase = m_fmt_ctx->streams[m_audioStream]->time_base;
    m_videoTimebase = m_fmt_ctx->streams[m_videoStream]->time_base;

    return tmpRet;
}

void DemuxThread::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QThread::start();
    if (m_videoDecodeThread) m_videoDecodeThread->start();
    if (m_audioThread) m_audioThread->start();//启动两个子线程

}

void DemuxThread::setPause(bool isPause)
{
    bool flag = false;
    if(m_isFrameStep){
        flag = true;
    }
    endFrameStep();
    m_isFrameStep.store(false);
    m_lastIsPause.store(m_isPause);
    m_isPause.store(isPause);
    if (m_audioThread) m_audioThread->setPause(isPause);
    if (m_videoDecodeThread) m_videoDecodeThread->setPause(isPause);
    if(flag){
        double pos = (double)getVideoPts() / totalMs;
        seek(pos);
    }
}

void DemuxThread::startFrameStep()
{
    if (!m_videoDecodeThread) return;
    //暂停音频与视频渲染线程  解码线程不停止
    // 音频暂停（声音停）
    //if (m_audioThread) m_audioThread->setPause(true);
    m_saveVolume = m_audioThread->getVolume();
    double re = 0.0;
    m_audioThread->setVolume(re);
    // 视频只暂停渲染，解码继续
    m_videoDecodeThread->setRenderPause(true);
    m_isFrameStep.store(true);

}

void DemuxThread::endFrameStep()
{
    if(!m_isFrameStep){
        return ;
    }
    m_isFrameStep.store(false);
    //将视频pts传给音频
    if (m_audioThread){
        long long pts = m_videoDecodeThread->getVideoRenderPts();
        m_audioThread->sendPts(pts);
        //解除暂停
        m_audioThread->setPause(false);
    }
    // 解除暂停
    m_videoDecodeThread->setRenderPause(false);
    m_audioThread->setVolume(m_saveVolume);
}

void DemuxThread::stepNextFrame()
{
    if(!m_isFrameStep.load()){
        startFrameStep();
    }
    m_videoDecodeThread->setStepFrameMode(1);
}

void DemuxThread::stepPrevFrame()
{
    if(!m_isFrameStep.load()){
        startFrameStep();
    }
    m_videoDecodeThread->setStepFrameMode(2);
}

bool DemuxThread::seek(double pos)
{
    if(pos < 0|| pos > 1|| m_isFrameStep ) {
        return false;
    }
    m_lastIsPause.store(m_isPause);
    setPause(false);
    QThread::usleep(500);
    emit disableBtn();
    m_seekPos = pos;
    m_isSeeking = true;
    m_serial.fetch_add(1); //serial++
    return true;
}


void DemuxThread::close()
{
    m_isExit = true;
    setPause(true);  // 先暂停
    closeAVThread();// 关闭音视频线程
    clear();         // 清空队列
    wait();         // 等 demux 线程退出
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
    if (m_videoDecodeThread) m_videoDecodeThread->clear();
    if (m_audioThread) m_audioThread->clear();
}

void DemuxThread::closeAVThread()
{
    if (m_videoDecodeThread) m_videoDecodeThread->close();
    if (m_audioThread) m_audioThread->close();
}

AVPacket *DemuxThread::readPkt()
{
    //qDebug()<<"readPkt";
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

    return pkt;
}

bool DemuxThread::getIsPause()
{
    return m_isPause;
}

long long DemuxThread::getVideoPts()
{
    return m_videoDecodeThread->getVideoRenderPts();
}

void DemuxThread::run()
{
    while(!m_isExit){
        // ===== 1. 先处理 seek  =====
        if (m_isSeeking)
        {
            m_isSeeking = false;
            // 1先保存暂停状态
            bool wasPause = m_lastIsPause;
            setPause(true);

            //清空两个 packet 队列和 frame 队列
            m_videoDecodeThread->clear();
            m_audioThread->clear();

            //2.seek
            int64_t seekMs = m_seekPos * totalMs;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!isCompleteInit || !m_fmt_ctx || m_videoStream < 0) {
                    setPause(wasPause);
                    emit ableBtn();
                    continue;
                }
                avformat_flush(m_fmt_ctx);
                int64_t ts = av_rescale_q(seekMs, {1, 1000}, m_videoTimebase);
                av_seek_frame(m_fmt_ctx, m_videoStream, ts, AVSEEK_FLAG_BACKWARD);
                // 解码器 flush
                m_videoDecodeThread->flushBuf();
                m_audioThread->flushBuf();
            }
            int serial = m_serial.load();
            while (!m_isExit)
            {
                AVPacket *pkt = readPkt(); // 内部自己加锁、快速释放
                if (!pkt) break;

                if (pkt->stream_index == m_videoStream) {
                    // repaintPts 内部只在解码瞬间加锁
                    bool found = m_videoDecodeThread->repaintPts(pkt, seekMs,serial);
                    if (found) break;
                } else {
                    av_packet_free(&pkt);
                }
            }
            // 3. 更新 serial
            m_audioThread->setSerial(serial);
            //m_audioThread->sendPts(seekMs);

            //复位音频重采样器与 atempo 滤镜
            m_audioThread->requestFilterReset();

            // 6. 恢复暂停状态
            setPause(wasPause);
            emit ableBtn();
            continue;
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
            if (m_videoDecodeThread && m_audioThread)
            {
                tmp = true;
            }
        }
        if(tmp){
            pts.store(m_audioThread->getPts());
            m_videoDecodeThread->setSynpts(pts);
        }
        AVPacket *pkt = readPkt();
        if (!pkt)
        {
            if(m_eof){
                m_videoDecodeThread->setLastSome(true);
                if(m_videoDecodeThread->getPlayDone()){
                    m_videoDecodeThread->setLastSome(false);
                    m_eof.store(false);
                    // 直接 seek 到开头 或下一集
                    if(m_hasPlayList){
                        emit playNext();
                    }else{
                        seek(0.0);
                    }
                }
            }
            msleep(5);
            continue;
        }
        static int a = 0;
        // 判断数据是音频
        if(pkt->stream_index == m_videoStream && m_videoDecodeThread){
            //视频
            m_videoDecodeThread->push(pkt,m_serial.load());
        }else if(pkt->stream_index == m_audioStream && m_audioThread){
            //音频
            m_audioThread->push(pkt,m_serial.load());
        }else{
            av_packet_free(&pkt);
        }
        //qDebug()<<"mutexThread->push";
        msleep(2);
    }
}

void DemuxThread::setDone()
{
    playDone.store(true);
}

bool DemuxThread::getIsExit() const
{
    return m_isExit.load();
}

void DemuxThread::setVolume(double& pos)
{
    if(m_audioThread){
        m_audioThread->setVolume(pos);
    }
}

void DemuxThread::setHasPlayList(bool has)
{
    m_hasPlayList.store(has);
}

void DemuxThread::setSpeed(double speed)
{    
    if (m_audioThread)       m_audioThread->setSpeed(speed);// 音频 atempo
    if (m_videoDecodeThread) m_videoDecodeThread->setSpeed(speed); // 视频帧时长
}



