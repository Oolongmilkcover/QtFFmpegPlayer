#include "audiothread.h"
#include <QDebug>
#include <algorithm>
extern"C"{
#include "libavcodec/codec_par.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"

//倍速用到的 avfilter 头
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/channel_layout.h"
}

AudioThread::AudioThread(int frameQueSize , bool keep_last )
{
    m_frameQue = new FrameQueue(frameQueSize,keep_last);
    m_pktQue = new PacketQueue();
    m_auPlayer = AudioPlayer::getPlayer();
    m_isExit = true;
}

AudioThread::~AudioThread()
{

    close();
    delete m_pktQue;
    delete m_frameQue;
    m_pktQue  = nullptr;
    m_frameQue = nullptr;
}

bool AudioThread::open(AVStream *audioStream)
{
    if(!audioStream){
        return false;
    }
    close();
    m_audioStream = audioStream;
    AVCodecParameters *para = m_audioStream->codecpar;

    std::lock_guard<std::mutex> lock(m_auMutex);
    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
        m_swr_ctx = nullptr;
    }
    m_auPlayer->sampleRate = m_outSampleRate;
    m_auPlayer->channels = m_outChannels;
    pts = 0;
    m_audioPts = 0;
    m_serial.store(0);
    //音频解码器初始化
    if(!codecInit(para)){
        qDebug()<<"audioDecodeInit failed!" ;
        return false;
    }
    //重采样初始化
    if(!resampleInit()){
        qDebug()<<"resampleInit  failed!" ;
        return false;
    }
    //音频播放器初始化
    if(!m_auPlayer->open()){
        qDebug()<<"audioPlayerInit  failed!" ;
        return false;
    }
    // 初始化倍速滤镜（默认 1.0 倍）
    if (!initAtempoFilter(1.0)) {
        qDebug() << "initAtempoFilter failed!";
        return false;
    }

    return true;
}

void AudioThread::close()
{

    m_isExit = true;
    m_running = false;

    if (m_pktQue)   m_pktQue->abort();
    if (m_frameQue) m_frameQue->abort();

    if (m_decodeThread.joinable())
        m_decodeThread.join();

    if (m_playThread.joinable())
        m_playThread.join();


    DecodeThread::close();

    std::lock_guard<std::mutex> lock(m_auMutex);
    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
        m_swr_ctx = nullptr;
    }

    freeAtempoFilter();

    if (m_auPlayer) {
        m_auPlayer->close();
    }
    m_audioPts.store(0);

    m_isPause = false;
}

void AudioThread::clear()
{
    if (m_pktQue)   m_pktQue->clear();
    if (m_frameQue) m_frameQue->clear();
    //if (m_auPlayer) m_auPlayer->clear();
    pts = 0;
    m_audioPts.store(0);
}

void AudioThread::setPause(bool isPause)
{
    m_isPause = isPause;
    if (m_auPlayer)
        m_auPlayer->setPause(isPause);
}

void AudioThread::startAudio()
{
    // 防止重复启动
    if (m_running || !m_isExit) return;

    // 如果之前的线程还存在，先等待
    if (m_decodeThread.joinable())
        m_decodeThread.join();

    if (m_playThread.joinable())
        m_playThread.join();

    // 开始运行
    m_pktQue->reset();
    m_frameQue->reset();
    m_isExit = false;
    m_running = true;

    // 启动解码线程
    m_decodeThread = std::thread(
        &AudioThread::decodeRun,
        this
        );

    // 启动播放线程
    m_playThread = std::thread(
        &AudioThread::playRun,
        this
        );

    qDebug() << "Audio decode thread started";
    qDebug() << "Audio play thread started";
}

void AudioThread::sendPts(int64_t ptsMs)
{
    pts.store(ptsMs);
    m_audioPts.store(ptsMs);
}

long long AudioThread::getPts()
{
    if (!m_auPlayer) return 0;
    // 最后写入的帧 pts"
    long long lastPts = m_audioPts.load();
    // 声卡未播时长
    long long noPlayReal = m_auPlayer->getNoPlayMs();
    // 换算成内容轴时长：输出样本 × speed = 内容样本
    long long noPlayContent = (long long)((double)noPlayReal
                       * m_currentSpeed.load());
    long long clock = lastPts - noPlayContent;
    if (clock < 0) clock = 0;
    pts.store(clock);
    // qDebug()<<"clock"<<clock;
    return clock;
}

void AudioThread::run()
{
    startAudio();
}

void AudioThread::setSerial(int serial)
{
    m_serial.store(serial);
}

void AudioThread::setVolume(double& pos)
{
    if(m_auPlayer){
        qreal tmpPos = (qreal)pos;
        m_auPlayer->setVolume(tmpPos);
    }
}

double AudioThread::getVolume()
{
    return m_auPlayer->getVolume();
}



void AudioThread::decodeRun()
{
    qDebug() << "Audio decode thread running...";
    while (!m_isExit)
    {
        //seek 后的复位：清 swr 延迟 + 重建 atempo（丢弃 seek 前残留）
        if (m_needFilterReset.exchange(false)) {
            std::lock_guard<std::mutex> lock(m_auMutex);
            if (m_swr_ctx) {
                // swr_init 会重置内部缓冲和延迟
                swr_init(m_swr_ctx);
            }
            // 重建滤镜 = 丢弃 seek 前 atempo 内部的旧样本
            initAtempoFilter(m_currentSpeed);
        }

        //速度变化时在解码线程里重建滤镜
        double desired = m_desiredSpeed.load();
        if (desired != m_currentSpeed.load()) {
            qDebug() << "speed change:" << m_currentSpeed << "->" << desired;
            if (initAtempoFilter(desired)) {
                // 重建成功，丢弃旧滤镜内部的残留数据（可接受）
                std::lock_guard<std::mutex> lock(m_auMutex);
                if (m_swr_ctx) {
                    // swr_init 会重置内部缓冲和延迟
                    swr_init(m_swr_ctx);
                }
            } else {
                // 重建失败：把目标速度改回当前值，避免死循环重试
                m_desiredSpeed.store(m_currentSpeed);
                qDebug() << "重建失败：把目标速度改回当前值，避免死循环重试" << desired;
            }
        }

        // 1. 从 PacketQueue 获取 AVPacket（原逻辑不变）
        auto packet = m_pktQue->pop(true);
        if (!packet) {
            if (m_isExit) break;
            continue;
        }
        int pktSerial = packet->m_serial;

        if (!send(packet)) continue;

        // 3. 一个 packet 可能解码出多个 AVFrame
        while (!m_isExit) {
            AVFrame *decodedFrame = recv();
            if (!decodedFrame) break;

            // seek 之后的旧 packet 直接丢弃（原逻辑）
            if (pktSerial != m_serial.load()) {
                av_frame_free(&decodedFrame);
                continue;
            }

            // 计算输入 pts（毫秒）
            int64_t raw = decodedFrame->pts;
            if (raw == AV_NOPTS_VALUE) raw = decodedFrame->best_effort_timestamp;
            if (raw == AV_NOPTS_VALUE) raw = 0;
            int64_t framePtsMs = av_rescale_q(raw, m_audioStream->time_base, {1, 1000});

            // 先重采样到临时帧，再送入 atempo
            AVFrame *resampled = av_frame_alloc();
            if (!resampled) {
                av_frame_free(&decodedFrame);
                break;
            }

            // 设置输出格式
            resampled->format = m_outSampleFmt;
            av_channel_layout_default(&resampled->ch_layout, m_outChannels);
            resampled->sample_rate = m_outSampleRate;

            // 计算输出容量
            int maxSamples = av_rescale_rnd(
                                 swr_get_delay(m_swr_ctx, m_codec_ctx->sample_rate)
                                     + decodedFrame->nb_samples,
                                 m_outSampleRate, m_codec_ctx->sample_rate, AV_ROUND_UP) + 256;
            resampled->nb_samples = maxSamples;

            if (av_frame_get_buffer(resampled, 0) < 0) {
                av_frame_free(&resampled);
                av_frame_free(&decodedFrame);
                continue;
            }

            // 重采样
            int samples = swr_convert(m_swr_ctx,
                                      resampled->data, resampled->nb_samples,
                                      (const uint8_t **)decodedFrame->data,
                                      decodedFrame->nb_samples);
            if (samples <= 0) {
                av_frame_free(&resampled);
                av_frame_free(&decodedFrame);
                continue;
            }
            resampled->nb_samples = samples;

            // 重采样延迟补偿（原来的 outPts 逻辑）
            int64_t delayIn  = swr_get_delay(m_swr_ctx, m_codec_ctx->sample_rate);
            int64_t delayOut = av_rescale_rnd(delayIn, m_outSampleRate,
                                              m_codec_ctx->sample_rate, AV_ROUND_UP);
            resampled->pts = framePtsMs - (delayOut * 1000LL / m_outSampleRate);

            av_frame_free(&decodedFrame);

            if (!m_ptsAnchorReady) {
                m_ptsBase = resampled->pts;   // 第一帧输入的内容 pts（毫秒）
                m_ptsAnchorReady = true;
            }

            // 送入 atempo 滤镜
            // atempo 有内部缓冲：
            //  - 2 倍速时，可能送两帧才吐一帧
            //  - 输出帧的 pts 由滤镜按处理样本数自动推算（毫秒）
            if (av_buffersrc_add_frame(m_buffersrcCtx, resampled) < 0) {
                qDebug() << "buffersrc_add_frame failed";
                av_frame_free(&resampled);
                continue;
            }
            // add_frame 成功后滤镜持有引用，我们释放自己的引用
            av_frame_free(&resampled);

            // 从滤镜取输出帧，可能有 0 帧或多帧
            while (true) {
                // qDebug()<<"AVFrame *outFrame = av_frame_alloc() 前";
                AVFrame *outFrame = av_frame_alloc();
                if (!outFrame) break;

                int ret = av_buffersink_get_frame(m_buffersinkCtx, outFrame);
                if (ret == AVERROR(EAGAIN)) {
                    // 滤镜内部缓冲还不够，这轮没输出
                    av_frame_free(&outFrame);
                    break;
                } else if (ret == AVERROR_EOF) {
                    // 滤镜结束（正常播放不会走到这里）
                    av_frame_free(&outFrame);
                    break;
                } else if (ret < 0) {
                    av_frame_free(&outFrame);
                    break;
                }

                // 拿到一帧变速后的音频，写入 FrameQueue
                Frame *frame = m_frameQue->getWritable();
                if (!frame) {
                    // 队列满了（背压）：丢弃这一帧，退出取帧循环
                    av_frame_free(&outFrame);
                    break;
                }

                //把"播放轴 pts"重映射为"内容轴 pts"
                // atempo 输出帧的 pts 按输出样本数累加（播放轴），
                // 2 倍速时它只有内容轴的一半，会和视频 pts 错位，
                // 导致视频渲染线程 diff 恒大于阈值 → 帧队列堵死 → 背压冻结。
                // 内容轴 pts = 基准 + (本帧之前已输出的样本数 × speed) 换算的时长
                m_outSamplesTotal += outFrame->nb_samples;
                outFrame->pts = m_ptsBase +
                                (int64_t)((double)(m_outSamplesTotal - outFrame->nb_samples)
                                           * m_currentSpeed.load()
                                           * 1000.0 / m_outSampleRate);

                av_frame_unref(frame->m_frame);
                av_frame_move_ref(frame->m_frame, outFrame);
                av_frame_free(&outFrame);

                // 输出帧的 pts 已经是毫秒（abuffer 的 time_base=1/1000）
                // 这里无需再换算
                frame->m_serial = pktSerial;
                m_frameQue->push();
            }
        }
        msleep(1);
    }
    qDebug() << "Audio decode thread end...";
}

void AudioThread::playRun()
{
    qDebug() << "Audio play thread running...";

    while (!m_isExit)
    {
        if (m_isPause.load())
        {
            msleep(1);
            continue;
        }
        Frame *frame = m_frameQue->getReadable();
        if (!frame)
        {
            if (m_isExit)
                break;

            continue;
        }

        // serial检查
        if (frame->m_serial != m_serial.load())
        {
            m_frameQue->next();
            continue;
        }
        AVFrame* audioFrame = av_frame_clone(frame->m_frame);
        int thisSerial = frame->m_serial;

        if (!audioFrame || audioFrame->nb_samples <= 0)
        {
            av_frame_free(&audioFrame);
            m_frameQue->next();
            qDebug()<<"!audioFrame || audioFrame->nb_samples <= 0";
            continue;
        }

        int bytesPerSample =
            av_get_bytes_per_sample(
                static_cast<AVSampleFormat>(audioFrame->format)
                );

        if (bytesPerSample <= 0)
        {
            m_frameQue->next();
            qDebug()<<"bytesPerSample <= 0";
            continue;
        }

        int dataSize =
            audioFrame->nb_samples *
            m_outChannels *
            bytesPerSample;

        // 等待声卡有足够空间
        int writtenTotal = 0;
        // while (!m_isExit&&!m_isPause)
        // {
        while (!m_isExit)
        {
            if(m_isPause){
                msleep(3);
                continue;
            }
            int remain = dataSize - writtenTotal;
            if (remain <= 0)
                break;

            int freeSize = m_auPlayer->getFree();

            if (freeSize <= 0)
            {
                msleep(1);
                continue;
            }

            int writeSize = std::min(remain, freeSize);

            int written = m_auPlayer->write(
                audioFrame->data[0] + writtenTotal,
                writeSize
                );

            if (written <= 0)
            {
                msleep(1);
                continue;
            }

            writtenTotal += written;
        }

        if (m_isExit)
            break;
        long long frameDurationMs =
            (long long)((double)audioFrame->nb_samples
                                                 * m_currentSpeed.load()
                                                 * 1000.0 / m_outSampleRate);
        //音频同步  可能是seek回来后这时候serial不同
        if(thisSerial != m_serial){
            //这里是seek回来的情况
            //demux已经设置过pts了
        }else{
            m_audioPts.store(audioFrame->pts + frameDurationMs);
        }
        av_frame_free(&audioFrame);
        // 消费Frame
        m_frameQue->next();
    }
    qDebug() << "Audio play thread end...";
}



bool AudioThread::resampleInit()
{
    //锁内调用
    //音频重采样
    //输入：解码出来的音频参数
    const AVChannelLayout* in_ch_layout = &m_codec_ctx->ch_layout;
    AVSampleFormat in_sample_fmt = m_codec_ctx->sample_fmt;
    int in_sample_rate = m_codec_ctx->sample_rate;
    //输出，声卡能播放的标准格式
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, m_outChannels);

    //重采样上下文
    m_swr_ctx = swr_alloc();
    int ret = swr_alloc_set_opts2(
        &m_swr_ctx,          // 重采样上下文

        &out_ch_layout,    // 输出声道布局
        m_outSampleFmt,    // 输出格式
        m_outSampleRate,   // 输出采样率

        in_ch_layout,      // 输入声道布局
        in_sample_fmt,     // 输入格式
        in_sample_rate,    // 输入采样率

        0, nullptr
        );
    if (ret < 0) {
        qDebug() << "重采样创建失败";
        return false;
    }
    //重采样初始化
    ret = swr_init(m_swr_ctx);
    if(ret<0){
        qDebug() << "重采样初始化失败";
        swr_free(&m_swr_ctx);  //失败必须释放！！！
        return false;
    }
    return true;
}

// 外部设置倍速：只记录目标值
// 真正重建滤镜在解码线程里做，避免跨线程操作滤镜图
void AudioThread::setSpeed(double speed)
{
    if (speed < 0.5) speed = 0.5;    // atempo 下限 0.5
    if (speed > 2.0) speed = 2.0;    // 单实例上限 2.0
    m_desiredSpeed.store(speed);
}


// 创建滤镜图：abuffer(输入) -> atempo(变速) -> abuffersink(输出)
bool AudioThread::initAtempoFilter(double speed)
{
    // 0. 如果已经有旧图，先释放
    freeAtempoFilter();
    // 1. 分配滤镜图
    m_filterGraph = avfilter_graph_alloc();
    if (!m_filterGraph) {
        qDebug() << "avfilter_graph_alloc failed";
        return false;
    }
    // 2. 获取滤镜工厂
    const AVFilter *srcFilter   = avfilter_get_by_name("abuffer");
    const AVFilter *tempoFilter = avfilter_get_by_name("atempo");
    const AVFilter *sinkFilter  = avfilter_get_by_name("abuffersink");
    // 3. 创建"源"滤镜，描述输入音频格式
    //    time_base=1/1000：之后帧的 pts 直接用毫秒
    //    格式与 swr 重采样输出一致：S16 / 48000 / stereo
    char srcArgs[256] = {0};
    snprintf(srcArgs, sizeof(srcArgs),
             "time_base=1/1000:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             m_outSampleRate,
             av_get_sample_fmt_name(m_outSampleFmt),
             m_outChannels == 2 ? "stereo" : "mono");
    if (avfilter_graph_create_filter(&m_buffersrcCtx, srcFilter, "in",
                                     srcArgs, nullptr, m_filterGraph) < 0) {
        qDebug() << "create abuffer failed";
        freeAtempoFilter();
        return false;
    }
    // 4. 创建 atempo 滤镜
    //    注意：tempoArgs 必须紧贴使用，不能和 goto/return 混用
    {
        char tempoArgs[64] = {0};
        snprintf(tempoArgs, sizeof(tempoArgs), "tempo=%.3f", speed);
        if (avfilter_graph_create_filter(&m_tempoCtx, tempoFilter, "tempo",
                                         tempoArgs, nullptr, m_filterGraph) < 0) {
            qDebug() << "create atempo failed";
            freeAtempoFilter();
            return false;
        }
    }   // tempoArgs 出了作用域就销毁，不影响后续
    // 5. 创建"汇"滤镜
    if (avfilter_graph_create_filter(&m_buffersinkCtx, sinkFilter, "out",
                                     nullptr, nullptr, m_filterGraph) < 0) {
        qDebug() << "create abuffersink failed";
        freeAtempoFilter();
        return false;
    }
    // 6. 连接：源 -> atempo -> 汇
    if (avfilter_link(m_buffersrcCtx, 0, m_tempoCtx, 0) < 0) {
        qDebug() << "link src->tempo failed";
        freeAtempoFilter();
        return false;
    }
    if (avfilter_link(m_tempoCtx, 0, m_buffersinkCtx, 0) < 0) {
        qDebug() << "link tempo->sink failed";
        freeAtempoFilter();
        return false;
    }
    // 7. 配置整个滤镜图（校验格式、分配内部缓冲）
    if (avfilter_graph_config(m_filterGraph, nullptr) < 0) {
        qDebug() << "avfilter_graph_config failed";
        freeAtempoFilter();
        return false;
    }
    m_currentSpeed.store(speed);
    //  下一帧送入滤镜的"实际输入 pts"才是内容基准
    m_outSamplesTotal = 0;
    m_ptsAnchorReady = false;

    qDebug() << "atempo filter ready, speed =" << speed;
    return true;
}

// 释放滤镜图（avfilter_graph_free 会连带释放里面的滤镜上下文）
void AudioThread::freeAtempoFilter()
{
    //std::lock_guard<std::mutex> lock(m_mutex);
    if (m_filterGraph) {
        avfilter_graph_free(&m_filterGraph);
        m_filterGraph = nullptr;
    }
    // 这些指针是 graph 内部的，graph 释放后置空即可
    m_buffersrcCtx  = nullptr;
    m_tempoCtx      = nullptr;
    m_buffersinkCtx = nullptr;
}
