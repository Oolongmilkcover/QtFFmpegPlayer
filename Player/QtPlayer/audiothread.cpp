#include "audiothread.h"
#include <QDebug>
#include <algorithm>
extern"C"{
#include "libavcodec/codec_par.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
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
    if (m_auPlayer) m_auPlayer->clear();
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
    long long noPlay = m_auPlayer->getNoPlayMs();
    long long clock = lastPts - noPlay;
    if (clock < 0) clock = 0;
    pts.store(clock);
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
        // 1. 从 PacketQueue 获取AVPacket
        //qDebug() << "AudioThread::decodeRun m_pktQue.size()"<<m_pktQue->size();
        auto packet = m_pktQue->pop(true);
        if (!packet)
        {
            if (m_isExit)
                break;

            continue;
        }

        // 当前packet属于哪个serial
        int pktSerial = packet->m_serial;

        // 2. 发送给FFmpeg解码器
        if (!send(packet))
        {
            continue;
        }
        // 3. 一个packet可能解码出多个AVFrame
        while (!m_isExit)
        {
            AVFrame *decodedFrame = recv();

            if (!decodedFrame)
            {
                break;
            }

            // seek之后的旧packet直接丢弃
            if (pktSerial != m_serial.load())
            {
                av_frame_free(&decodedFrame);
                continue;
            }
            int64_t raw = decodedFrame->pts;
            if (raw == AV_NOPTS_VALUE) raw = decodedFrame->best_effort_timestamp;
            if (raw == AV_NOPTS_VALUE) raw = 0;
            int64_t framePtsMs = av_rescale_q(raw, m_audioStream->time_base, {1, 1000});

            // 4. 从FrameQueue获取一个可写位置
            Frame *frame = m_frameQue->getWritable();
            if (!frame)
            {
                av_frame_free(&decodedFrame);
                break;
            }

            // 清空旧数据
            av_frame_unref(frame->m_frame);

            // 5. 设置输出Frame格式
            frame->m_frame->format = m_outSampleFmt;

            av_channel_layout_default(
                &frame->m_frame->ch_layout,
                m_outChannels
                );

            frame->m_frame->sample_rate = m_outSampleRate;

            // 输出容量
            int maxSamples = av_rescale_rnd(
                swr_get_delay(
                    m_swr_ctx,
                    m_codec_ctx->sample_rate
                    ) + decodedFrame->nb_samples,
                m_outSampleRate,
                m_codec_ctx->sample_rate,
                AV_ROUND_UP
                )+256;

            frame->m_frame->nb_samples = maxSamples;

            // 分配输出buffer
            if (av_frame_get_buffer(frame->m_frame, 0) < 0)
            {
                av_frame_unref(frame->m_frame);
                av_frame_free(&decodedFrame);
                continue;
            }

            // 6. 重采样
            int samples = swr_convert(
                m_swr_ctx,
                frame->m_frame->data,
                frame->m_frame->nb_samples,

                (const uint8_t **)decodedFrame->data,
                decodedFrame->nb_samples
                );

            if (samples <= 0)
            {
                av_frame_unref(frame->m_frame);
                av_frame_free(&decodedFrame);
                continue;
            }

            // 7. 设置实际样本数
            frame->m_frame->nb_samples = samples;

            // 获取重采样内部延迟（输出采样率下的样本数）
            int64_t delayIn = swr_get_delay(m_swr_ctx, m_codec_ctx->sample_rate);
            int64_t delayOut = av_rescale_rnd(delayIn, m_outSampleRate, m_codec_ctx->sample_rate, AV_ROUND_UP);
            // 输出帧的起始pts = 输入pts - 延迟对应的毫秒数
            int64_t outPts = framePtsMs - (delayOut *1000LL / m_outSampleRate);
            frame->m_frame->pts = outPts;


            // 9. 保存serial
            frame->m_serial = pktSerial;

            // 10. AVFrame已经复制进FrameQueue
            av_frame_free(&decodedFrame);

            // 11. 通知消费者
            m_frameQue->push();
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
            qDebug() << "m_isPause";
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
        long long frameDurationMs = audioFrame->nb_samples * 1000LL / m_outSampleRate;
        //音频同步  可能是seek回来后这时候serial不同
        if(thisSerial != m_serial){
            //这里是seek回来的情况
            //demux已经设置过了
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




