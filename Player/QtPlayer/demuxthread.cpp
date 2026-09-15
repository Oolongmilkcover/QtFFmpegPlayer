#include "demuxthread.h"
#include "audiothread.h"
#include "videodecodethread.h"
#include "videorenderthread.h"
#include "videowidget.h"
#include <QDebug>

extern "C" {
#include <libavformat/avformat.h>
}

//往解码线程的包队列里放包时最多等多久（毫秒）
//用带超时的等待，保证 demux 线程随时能响应 seek / 退出 / 逐帧回填
static constexpr int PUSH_TIMEOUT_MS = 10;

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
    m_seekPauseing.store(false); //兜底：清掉上一次可能残留的“seek 进行中”

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
            if (m_fmt_ctx) {
                avformat_close_input(&m_fmt_ctx);
                m_fmt_ctx = nullptr;
            }
            return false;
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
            if (m_videoStream < 0 && m_audioStream < 0){
                qDebug() << "没有找到任何音视频流";
                // 释放已创建的线程
                closeAVThread();
                tmpRet = false;
            }
            // 记录流是否存在（后面判断用）
            m_hasVideo = (m_videoStream >= 0);
            m_hasAudio = (m_audioStream >= 0);

            //判断是否为音频文件 是就关闭seek
            m_disableSeekFlag = false;
            m_containerName.clear();
            if(m_fmt_ctx->iformat != nullptr)
            {

                m_containerName = QString::fromUtf8(m_fmt_ctx->iformat->name);
                qDebug()<<"容器名称(iformat->name): "<<m_containerName;
                //容器属于纯音频集合
                if(m_audioOnlyFormat.contains(m_containerName.toLower()))
                {
                    m_disableSeekFlag = true;
                    qDebug()<<"检测到音频文件";
                }
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
    if (m_hasVideo){
        // 打开视频解码器和处理线程
        AVCodecParameters *vpara = m_fmt_ctx->streams[m_videoStream]->codecpar;
        m_width = vpara->width;
        m_height = vpara->height;
        if (m_hasVideo){
            if(!m_videoDecodeThread->open(widget,vpara->width,vpara->height,m_fmt_ctx->streams[m_videoStream])){
                tmpRet = false;
                qDebug()<<"m_videoDecodeThread->open failed";
            }
        }
    }
    if (m_hasAudio){
        //打开音频解码器和处理线程
        if(!m_audioThread->open(m_fmt_ctx->streams[m_audioStream])){
            qDebug() << "音频打开失败，降级为静音播放";
            m_hasAudio = false;
        }else{
            m_videoDecodeThread->setSynpts(0);
        }
    }
    m_videoDecodeThread->setHasAudio(m_hasAudio);
    if(!tmpRet){
        closeAVThread();
    }else{
        isCompleteInit = true;
        setPause(false);
        qDebug()<<"openFile success";
    }
    if (m_hasAudio) m_audioTimebase = m_fmt_ctx->streams[m_audioStream]->time_base;
    if (m_hasVideo) m_videoTimebase = m_fmt_ctx->streams[m_videoStream]->time_base;

    return tmpRet;
}

void DemuxThread::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QThread::start();
    if (m_hasVideo && m_videoDecodeThread) m_videoDecodeThread->start();
    if (m_hasAudio && m_audioThread) m_audioThread->start();//启动两个子线程

}

void DemuxThread::setPause(bool isPause)
{
    //逐帧期间用户点了播放/暂停 → 先退出逐帧（内部会把播放位置对齐）
    if (m_isFrameStep.load()) endFrameStep();

    //正好有 seek 在处理时先别恢复渲染/解码，免得闪过期帧（doSeek 结束会按这个状态恢复）
    bool hold = m_isSeeking.load();

    m_pauseAfterSeek.store(isPause);
    m_isPause.store(isPause);
    if (m_videoDecodeThread) m_videoDecodeThread->setPause(isPause || hold);
    if (m_hasAudio && m_audioThread) m_audioThread->setPause(isPause || hold);
}

void DemuxThread::startFrameStep()
{
    if (!m_hasVideo || !m_videoDecodeThread) return;
    if (m_isFrameStep.exchange(true)) return;      //已经在逐帧了

    //逐帧期间：demux 与解码线程继续工作，只暂停“渲染”
    m_pauseBeforeStep.store(m_isPause.load());
    m_isPause.store(false);
    m_videoDecodeThread->setPause(false);          //解码线程继续解码
    m_videoDecodeThread->setRenderPause(true);     //渲染线程停下来等逐帧命令
    //音频暂停（声音停），退出逐帧时会 seek 重新对齐音视频
    if (m_hasAudio && m_audioThread) m_audioThread->setPause(true);
}

void DemuxThread::endFrameStep()
{
    if (!m_isFrameStep.exchange(false)) return;

    //进入逐帧之前是什么暂停状态，退出后就应该回到什么状态
    const bool wasPause = m_pauseBeforeStep.load();

    bool needSeek = true;
    if (m_videoDecodeThread) {
        //只有“没有回退过 + 没有音频”才可能无缝接着播
        needSeek = !(m_videoDecodeThread->cursorAtNewest() && !m_hasAudio);
        m_videoDecodeThread->clearStepRequests();
    }

    if (needSeek && !m_isExit.load() && totalMs > 0 && m_hasVideo) {
        //逐帧后屏幕上的帧和 demux 的读位置大概率不一致 → seek 对齐
        //直接用“当前显示帧的毫秒 pts”，不要走 pos<->ms 的浮点换算，
        //否则可能多 1ms 而落到下一帧
        requestSeekMs(getVideoPts());
        setPause(wasPause);
    } else {
        setPause(wasPause);
    }
}

bool DemuxThread::stepNextFrame()
{
    if(!m_hasVideo || m_disableSeekFlag || !m_videoDecodeThread) return false;
    if(!m_isFrameStep.load()){
        startFrameStep();
    }
    if(!m_isFrameStep.load()) return false;
    m_videoDecodeThread->requestStep(1);
    return true;
}

bool DemuxThread::stepPrevFrame()
{
    if(!m_hasVideo || m_disableSeekFlag || !m_videoDecodeThread) return false;
    if(!m_isFrameStep.load()){
        startFrameStep();
    }
    if(!m_isFrameStep.load()) return false;
    m_videoDecodeThread->requestStep(-1);
    return true;
}

bool DemuxThread::seek(double pos)
{
    if(pos < 0|| pos > 1) {
        return false;
    }
    //把比例换成毫秒后统一走 requestSeekMs
    return requestSeekMs((long long)(pos * (double)totalMs));
}

bool DemuxThread::seekToMs(long long ms)
{
    return requestSeekMs(ms);
}

bool DemuxThread::requestSeekMs(long long ms)
{
    if(ms < 0) ms = 0;
    if(totalMs > 0 && ms > totalMs) ms = totalMs;

    //逐帧中先退出逐帧（否则读位置和显示位置会对不上）
    if (m_isFrameStep.load()) endFrameStep();


    m_seekMs.store(ms);
    m_serial.fetch_add(1);
    //纯音频（MP3等）：底层直接 seek
    if (m_disableSeekFlag) {
        bool wasPause = m_isPause.load();
        m_seekPauseing.store(true);
        setPause(true); // 先暂停
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_fmt_ctx && m_hasAudio) {
                // 清空音频队列
                m_audioThread->clear();
                // 注意：clear() 会把音频时钟(m_audioPts)一起清零，而进度条读的就是这个时钟，
                // 于是 seek 后会闪一下“回到开头”，连环快进也会因为读到 0 而永远算成 0+5000。
                // 这里只是把时钟锚到本次 seek 目标；等新音频解码出来，sendPts 会自然接管。
                m_audioThread->sendPts(ms);
                pts.store(ms);
                avformat_flush(m_fmt_ctx);
                int64_t ts = av_rescale_q(ms, {1, 1000}, m_audioTimebase);
                av_seek_frame(m_fmt_ctx, m_audioStream, ts, AVSEEK_FLAG_BACKWARD);
                // 解码器 flush，丢弃 seek 前的残留
                m_audioThread->flushBuf();
                // 重置重采样器 + atempo 滤镜（丢弃残留）
                m_audioThread->requestFilterReset();
                // 同步 serial
                m_audioThread->setSerial(m_serial.load());
            }
        }
        // 无论上面的 if 是否进去过，都必须清掉“seek 进行中”：
        // 否则 m_fmt_ctx 为空 / 没有音频流时它会永久停在 true，
        // player.cpp 的 queueSeekBy 开头会直接 return，快进/快退彻底失效。
        m_seekPauseing.store(false);
        setPause(wasPause);
        return true;
    }

    //其余情况
    //注意：这里不改暂停状态，doSeek() 会按 m_pauseAfterSeek 保存/恢复，
    //否则用户在暂停时拖动进度条会被“恢复播放”。
    //
    //但采样必须跳过“seek 正在执行中”的窗口：doSeek 为了独占生产者会把 m_isPause
    //临时置 true 再恢复。长按方向键时下一次请求正好落进那个窗口，照抄就会把这份
    //临时状态当成用户意图存进 m_pauseAfterSeek，seek 结束后播放器卡在暂停
    //（现象：长按快进/快退后要按两次播放键；纯音频走上面的直连分支所以没有）。
    if (!m_isSeeking.load()) {
        m_pauseAfterSeek.store(m_isPause.load());
    }
    emit disableBtn();
    m_isSeeking = true;
    return true;
}


void DemuxThread::close()
{
    m_isExit = true;
    //退出逐帧（这里不做位置对齐，马上要关了）
    m_isFrameStep.store(false);
    m_isSeeking.store(false);
    setPause(true);  // 先暂停
    closeAVThread();// 关闭音视频线程
    clear();         // 清空队列
    wait();         // 等 demux 线程退出
    //demux 线程已经退出，这时处理残留的包才安全
    if(m_pendingPkt){
        av_packet_free(&m_pendingPkt);
        m_pendingPkt = nullptr;
    }
    m_disableSeekFlag = false;
    m_containerName.clear();
    m_seekPauseing.store(false);   // 兜底：别把“seek 进行中”带进下一次播放
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fmt_ctx) {
        avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }
    m_videoStream = -1;
    m_audioStream = -1;
    totalMs = 0;
    isCompleteInit = false;
    m_hasVideo = false;
    m_hasAudio = false;
    pts = 0;
}

void DemuxThread::clear()
{
    if (m_videoDecodeThread) m_videoDecodeThread->clear();
    if (m_audioThread) m_audioThread->clear();
}

void DemuxThread::closeAVThread()
{
    if (m_hasVideo && m_videoDecodeThread) m_videoDecodeThread->close();
    if (m_hasAudio && m_audioThread) m_audioThread->close();
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
        //逐帧回填：历史帧退到头了，需要向后解码一段补进历史
        if(m_hasVideo && m_videoDecodeThread && m_videoDecodeThread->needRefill.load()){
            doBackwardRefill();
            continue;
        }

        // 处理 seek
        // 注意：这里不再用 exchange(false) 提前清标志——m_isSeeking 现在表示
        // “seek 正在进行中”，必须一直保持到 doSeek() 结束：
        // setPause() 用它作 hold（“seek 处理中先别恢复解码/渲染”），
        // requestSeekMs() 用它跳过“doSeek 临时暂停”的窗口。
        if (m_isSeeking.load())
        {
            doSeek();
            continue;
        }

        // 暂停或未初始化 → 等待
        if(m_isPause || !isCompleteInit)
        {
            msleep(5);
            continue;

        }
        // 时钟
        if (m_isFrameStep.load()) {
            //逐帧：进度条跟着屏幕上那一帧走
            if (m_hasVideo && m_videoDecodeThread)
                pts.store(m_videoDecodeThread->getVideoRenderPts());
        } else if (m_hasAudio) {
            pts.store(m_audioThread->getPts());
            if (m_hasVideo) m_videoDecodeThread->setSynpts(pts);
        } else if (m_hasVideo) {
            // 视频自己做主时钟
            pts.store(m_videoDecodeThread->getVideoRenderPts());
        }

        // 上一次没推进队列的包优先重试
        AVPacket *pkt = m_pendingPkt;
        m_pendingPkt = nullptr;
        if (!pkt) pkt = readPkt();
        if (!pkt)
        {
            //已经快结束了准备下一集或者重播
            if(m_eof && !m_isFrameStep.load()){
                //有视频时
                if(m_hasVideo && !m_disableSeekFlag){
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
                //只有音频时
                if ((m_hasAudio && !m_hasVideo)|| m_disableSeekFlag) {
                    if (m_audioThread->isPlayFinished()) {
                        // 播完了
                        m_eof.store(false);
                        // 直接 seek 到开头 或下一集
                        if(m_hasPlayList){
                            emit playNext();
                        }else{
                            seek(0.0);
                        }
                    }else{
                        msleep(10);
                        continue;
                    }
                }
            }
            msleep(5);
            continue;
        }
        // 判断数据是音频
        bool pushed = true;
        if(m_hasVideo && pkt->stream_index == m_videoStream && m_videoDecodeThread){
            //视频
            pushed = m_videoDecodeThread->tryPush(pkt,m_serial.load(),PUSH_TIMEOUT_MS);
        }else if(m_hasAudio && pkt->stream_index == m_audioStream && m_audioThread){
            if(m_isFrameStep.load()){
                //逐帧期间音频不推进（退出逐帧时会seek重新对齐），直接丢
                av_packet_free(&pkt);
            }else{
                //音频
                pushed = m_audioThread->tryPush(pkt,m_serial.load(),PUSH_TIMEOUT_MS);
            }
        }else{
            av_packet_free(&pkt);
        }
        //队列满没推进去 → 留着下次重试，不能丢包
        if(!pushed) m_pendingPkt = pkt;
        //qDebug()<<"mutexThread->push";
        msleep(1);
    }

    if(m_pendingPkt){
        av_packet_free(&m_pendingPkt);
        m_pendingPkt = nullptr;
    }
}

void DemuxThread::doSeek()
{
    //过期数据丢掉
    if(m_pendingPkt){
        av_packet_free(&m_pendingPkt);
        m_pendingPkt = nullptr;
    }

    // 1先保存暂停状态（以“seek 结束后该恢复成什么状态”为准）
    bool wasPause = m_pauseAfterSeek.load();
    //seek期间先停生产
    m_isPause.store(true);
    m_seekPauseing.store(true);

    pts.store(m_seekMs.load());
    if (m_videoDecodeThread) m_videoDecodeThread->setPause(true);
    if (m_hasAudio && m_audioThread) m_audioThread->setPause(true);

    //清空两个 packet 队列和 frame 队列
    if(m_hasVideo) m_videoDecodeThread->clear();
    if(m_hasAudio) m_audioThread->clear();

    //2.seek（目标毫秒由 requestSeekMs 统一换算好）
    int64_t seekMs = m_seekMs.load();
    // 同上：上面的 clear() 已经把音频时钟清零，这里把它锚回本次 seek 目标，
    // 避免进度条闪回开头、以及连环快进读到 0 而一直算成 0+5000。
    if (m_hasAudio && m_audioThread) m_audioThread->sendPts(seekMs);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!isCompleteInit || !m_fmt_ctx || (!m_hasVideo && !m_hasAudio)) {
            m_isPause.store(wasPause);
            if (m_videoDecodeThread) m_videoDecodeThread->setPause(wasPause);
            if (m_hasAudio && m_audioThread) m_audioThread->setPause(wasPause);
            m_isSeeking.store(false);    // seek 结束，清“进行中”标志
            m_seekPauseing.store(false); // 早退也必须清，否则快进/快退会永久失效
            emit ableBtn();
            return;
        }
        avformat_flush(m_fmt_ctx);

        //seek也要判断是哪个主时钟
        if (m_hasVideo) {
            int64_t ts = av_rescale_q(seekMs, {1,1000}, m_videoTimebase);
            av_seek_frame(m_fmt_ctx, m_videoStream, ts, AVSEEK_FLAG_BACKWARD);
        } else if (m_hasAudio) {
            int64_t ts = av_rescale_q(seekMs, {1,1000}, m_audioTimebase);
            av_seek_frame(m_fmt_ctx, m_audioStream, ts, AVSEEK_FLAG_BACKWARD);
        }
        m_seekPauseing.store(false);

        // 解码器 flush
        if (m_hasVideo) m_videoDecodeThread->flushBuf();
        if (m_hasAudio) m_audioThread->flushBuf();
    }
    int serial = m_serial.load();
    while (m_hasVideo && !m_isExit)
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
    if (m_hasAudio){
        m_audioThread->setSerial(serial);
        //复位音频重采样器与 atempo 滤镜
        m_audioThread->requestFilterReset();
    }

    // 6. 恢复暂停状态
    m_isPause.store(wasPause);
    if (m_videoDecodeThread) m_videoDecodeThread->setPause(wasPause);
    if (m_hasAudio && m_audioThread) m_audioThread->setPause(wasPause);
    // seek 结束，清“进行中”标志：此后新的 seek 请求才能重新采样用户的暂停意图
    m_isSeeking.store(false);

    emit ableBtn();
}

void DemuxThread::doBackwardRefill()
{
    // //---
    // const qint64 startNs = perfNowNs();
    // qDebug() << "[PERF] doBackwardRefill entry, startNs = " << startNs <<" ns";
    // //----

    if(!m_videoDecodeThread){
        return;
    }
    if(!m_hasVideo || !m_fmt_ctx){
        m_videoDecodeThread->finishRefill(false);
        return;
    }

    m_videoDecodeThread->needRefill.store(false);
    const int64_t boundaryMs = m_videoDecodeThread->refillBoundaryPts.load();

    //已经退出逐帧 / 没有有效边界 → 放弃这次回填
    if(!m_isFrameStep.load() || boundaryMs <= 0){
        m_videoDecodeThread->finishRefill(false);
        return;
    }

    if(m_pendingPkt){
        av_packet_free(&m_pendingPkt);
        m_pendingPkt = nullptr;
    }

    //1.丢掉待解码数据（保留历史帧），让解码线程让出解码器
    m_videoDecodeThread->clearForRefill();

    //2.向后多退一点，一次回填尽量补满一批历史帧
    double fps = m_videoDecodeThread->getFps();
    if(fps <= 0) fps = 25.0;
    long long spanMs = (long long)((FrameQueue::MAX_HISTORY_SIZE / 2) * 1000.0 / fps);
    if(spanMs < 500)  spanMs = 500;
    if(spanMs > 5000) spanMs = 5000;
    int64_t targetMs = boundaryMs - spanMs;
    if(targetMs < 0) targetMs = 0;

    m_serial.fetch_add(1);
    int serial = m_serial.load();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(!m_fmt_ctx){
            m_videoDecodeThread->finishRefill(false);
            return;
        }
        avformat_flush(m_fmt_ctx);
        int64_t ts = av_rescale_q(targetMs, {1,1000}, m_videoTimebase);
        av_seek_frame(m_fmt_ctx, m_videoStream, ts, AVSEEK_FLAG_BACKWARD);
    }
    //解码器 flush，serial 同步（渲染线程也要跟着）
    m_videoDecodeThread->flushBuf();
    m_videoDecodeThread->setSerial(serial);

    //3.从 targetMs 解码到 boundaryMs，把中间的帧回填进历史
    auto readVideoPkt = [this]() -> AVPacket* {
        while(!m_isExit){
            AVPacket* p = readPkt();
            if(!p) return nullptr;
            if(p->stream_index == m_videoStream) return p;
            av_packet_free(&p);
        }
        return nullptr;
    };
    bool ok = m_videoDecodeThread->refillBackward(boundaryMs, serial, readVideoPkt);
    m_videoDecodeThread->finishRefill(ok);

    // const qint64 endNs  = perfNowNs();
    // const qint64 costNs = endNs - startNs;
    // qDebug().nospace()
    //     << "[PERF] =====  回填耗时 = "
    //     << QString::number(costNs / 1e6, 'f', 2) << " ms";

    //4.回填期间用户退出了逐帧 → 读位置需要重新对齐
    if(!m_isFrameStep.load() && !m_isExit.load() && totalMs > 0){
        requestSeekMs(getVideoPts());
    }

}

void DemuxThread::setDone()
{
    playDone.store(true);
}

// 注意：这里必须和头文件里的声明完全一致（头文件是 long long）。
// Linux/glibc 下 int64_t 实际是 long，而 Windows/MSVC 下是 long long ——
// 两边写成不同类型时，这个定义会被当成"新的重载"，GCC 报
//   error: no declaration matches 'void DemuxThread::videoCallSeek(int64_t)'
void DemuxThread::videoCallSeek(long long ms)
{
    seekToMs(ms);
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
    m_speed.store(speed);
    if (m_hasAudio && m_audioThread)       m_audioThread->setSpeed(speed);// 音频 atempo
    if (m_hasVideo && m_videoDecodeThread)  m_videoDecodeThread->setSpeed(speed); // 视频帧时长
}



