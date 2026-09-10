#include "videodecodethread.h"
#include "videorenderthread.h"
#include"videowidget.h"
extern"C"{
#include "libavutil/frame.h"
#include "libavcodec/packet.h"
#include "libavcodec/avcodec.h"
}
#include <QDebug>

//一次回填最多保留多少帧（必须明显小于历史队列容量）
static constexpr int kRefillKeepFrames = 16;

/*
 * 把解码器给出的原始时间戳统一换算成毫秒。
 *
 * 注意：整个播放器里 AVFrame::pts 一律当“毫秒”用
 * （帧队列、历史队列、进度条、seek 都是毫秒），
 * 所以任何一条往队列里填帧的路径都必须在填之前换算一次，
 * 否则坐标系会串（表现就是 seek 触发位置却不对）。
 */
static int64_t framePtsMs(const AVFrame* f, AVRational timeBase)
{
    int64_t raw = f->best_effort_timestamp;
    if (raw == AV_NOPTS_VALUE) raw = f->pts;
    if (raw == AV_NOPTS_VALUE) raw = 0;
    return av_rescale_q(raw, timeBase, {1, 1000});
}

VideoDecodeThread::VideoDecodeThread(int frameSize, bool keep_last )
{
    m_frameQue = new FrameQueue(frameSize,keep_last);
    m_pktQue = new PacketQueue();
    m_videoRenderThread = new VideoRenderThread(m_frameQue);

    //历史耗尽 → 让 demux 线程向后 seek 回填（回调在渲染线程里执行，只写原子量）
    m_frameQue->onNeedRefill = [this](long long pts){
        refillBoundaryPts.store(pts);
        needRefill.store(true);
    };
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
    //上一轮的逐帧状态全部作废
    needRefill.store(false);
    refillBoundaryPts.store(-1);
    m_stepDecoding.store(false);
    m_videoStream = videoStream;
    AVCodecParameters *para = videoStream->codecpar;

    m_widget = widget;
    m_widget->Init(width,height);
    m_serial = 0;

    m_videoRenderThread->serial = 0;
    m_videoRenderThread->clearStepRequests();

    bool ret = codecInit(para);
    if (ret) {
        setFps(videoStream);
        m_videoRenderThread->setWidget(m_widget);
        m_videoRenderThread->restart();
        m_videoRenderThread->resetFrameClock();
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
    m_videoRenderThread->resetFrameClock();
    std::lock_guard<std::mutex> lock(m_viMutex);
    if (!m_codec_ctx) {
        av_packet_free(&pkt);
        return false;
    }
    /*
     * 逐帧要求“对准具体某一帧”，所以这里必须完整解码。
     * 之前用 skip_frame = AVDISCARD_NONREF 只解参考帧来加速，
     * 结果会停在目标时间之后的第一个 I/P 帧上（位置要偏几帧），
     * 退出逐帧/拖动进度条时看到的就是“seek 触发了但位置不对”。
     */
    // 发送 pkt（send 内部会释放 pkt）
    if (!send(pkt)) {
        return false;
    }

    bool found = false;

    while (!found && !m_isExit) {
        AVFrame* frame = recv();
        if (!frame) break;
        //统一换算成毫秒
        int64_t frameMs = framePtsMs(frame, m_videoStream->time_base);
        if (frameMs >= seekpts) {
            paint(frame);
            //记真正显示出来的那一帧的时间，而不是请求的时间
            m_videoRenderThread->pts.store(frameMs);
            found = true;
        } else {
            av_frame_free(&frame);
        }
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

        // 一包的 send+recv 必须连续完成，
        // 否则 seek / 逐帧回填会插进来，两边的帧会串
        std::lock_guard<std::mutex> gate(m_decodeGate);
        if (m_isExit) break;
        // 逐帧回填正在进行，或者这个包已经过期 → 丢掉
        if (m_stepDecoding.load() || pktSerial != m_serial.load()) {
            continue;
        }
        //开始解码
        if(!send(packet)){
            qDebug() << "!send(packet)";
            continue;
        }
        //一包多帧，recv到FrameQueue
        while(!m_isExit && !m_stepDecoding.load()){
            //得到一个解码后的帧
            AVFrame* avFrame = recv();
            if(!avFrame){
                break;
            }
            if(pktSerial != m_serial.load()){
                av_frame_free(&avFrame);
                break;
            }
            //统一换算成毫秒
            int64_t tmpPts = framePtsMs(avFrame, m_videoStream->time_base);
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

double VideoDecodeThread::getFps() const
{
    return m_videoRenderThread ? m_videoRenderThread->getFps() : 25.0;
}

void VideoDecodeThread::setHasAudio(bool flag)
{
    m_videoRenderThread->hasAudio = flag;
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

void VideoDecodeThread::requestStep(int delta)
{
    if (m_videoRenderThread)
        m_videoRenderThread->requestStep(delta);
}

void VideoDecodeThread::clearStepRequests()
{
    if (m_videoRenderThread)
        m_videoRenderThread->clearStepRequests();
    if (m_frameQue)
        m_frameQue->clearRefillPending();
    needRefill.store(false);
}

bool VideoDecodeThread::cursorAtNewest()
{
    return m_frameQue ? m_frameQue->cursorAtNewest() : true;
}

void VideoDecodeThread::clearForRefill()
{
    // 丢掉待解码的包和未来帧，但保留历史帧
    if (m_pktQue)   m_pktQue->clear();
    if (m_frameQue) m_frameQue->clearFuture();
}

bool VideoDecodeThread::refillBackward(int64_t boundaryMs, int serial,
                                       const std::function<AVPacket*()>& readPkt)
{
    if (!m_frameQue || !m_videoStream) return false;

    //先让解码线程退出当前包的 recv 循环
    m_stepDecoding.store(true);
    //独占解码器
    std::lock_guard<std::mutex> gate(m_decodeGate);

    //拿到闸门后再清一次未来队列：清掉解码线程刚才写进去的陈旧帧
    m_frameQue->clearFuture();

    if (!m_codec_ctx) {
        m_stepDecoding.store(false);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_originalSkip = m_codec_ctx->skip_frame;
        //逐帧回填要完整解码，否则会漏掉非参考帧
        m_codec_ctx->skip_frame = AVDISCARD_DEFAULT;
        avcodec_flush_buffers(m_codec_ctx);
    }

    std::deque<AVFrame*> batch;
    bool reached = false;

    while (!m_isExit && !reached) {
        AVPacket* pkt = readPkt ? readPkt() : nullptr;
        if (!pkt) break;                 // 到头了
        if (!send(pkt)) continue;        // send 内部释放 pkt

        while (!m_isExit) {
            AVFrame* f = recv();
            if (!f) break;

            //统一换算成毫秒
            int64_t ms = framePtsMs(f, m_videoStream->time_base);

            if (ms >= boundaryMs) {
                //边界帧本身已经在历史里了，丢掉它
                av_frame_free(&f);
                reached = true;
                break;
            }
            //历史队列里的 pts 必须是毫秒，否则下一次回填的 seek 目标就错了
            f->pts = ms;
            batch.push_back(f);
            //容量有限：只留最靠近边界的那些帧
            if ((int)batch.size() > kRefillKeepFrames) {
                av_frame_free(&batch.front());
                batch.pop_front();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_codec_ctx) m_codec_ctx->skip_frame = static_cast<AVDiscard>(m_originalSkip);
    }
    m_stepDecoding.store(false);

    const int got = (int)batch.size();
    bool ok = (got > 0);
    //回填进来的帧的 pts 范围（毫秒），方便和 seek 目标对照
    long long firstPts = ok ? batch.front()->pts : -1;
    long long lastPts  = ok ? batch.back()->pts  : -1;
    if (ok) {
        m_frameQue->prependHistory(batch, serial);
    } else {
        for (AVFrame*& f : batch) av_frame_free(&f);
        batch.clear();
    }
    qDebug() << "refillBackward: 边界(ms)" << (long long)boundaryMs
             << "回填" << got << "帧" << "范围(ms)[" << firstPts << "~" << lastPts << "]"
             << (reached ? "(到达边界)" : "(读到文件尾)")
             << "ok:" << ok;
    return ok;
}

void VideoDecodeThread::finishRefill(bool gotFrames)
{
    needRefill.store(false);
    if (m_frameQue) m_frameQue->finishRefill(gotFrames);
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

void VideoDecodeThread::setSpeed(double speed)
{
    m_videoRenderThread->setSpeed(speed);
}

void VideoDecodeThread::setSynpts(long long synpts)
{
    m_videoRenderThread->synpts.store(synpts);
}
