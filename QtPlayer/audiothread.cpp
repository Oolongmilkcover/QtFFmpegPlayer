#include "audiothread.h"
#include <QDebug>
extern"C"{
#include "libavcodec/codec_par.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
}

AudioThread::AudioThread()
{
    m_auPlayer = AudioPlayer::getPlayer();
}

AudioThread::~AudioThread()
{
    close();
}

bool AudioThread::open(AVCodecParameters *para, int sampleRate, int channels)
{
    if(!para){
        return false;
    }
    Clear();

    std::lock_guard<std::mutex> lock(m_auMutex);

    m_auPlayer->sampleRate = sampleRate;
    m_auPlayer->channels = channels;

    bool ret = true;
    pts = 0;
    //音频解码器初始化
    if(!codecInit(para)){
        qDebug()<<"audioDecodeInit failed!" ;
        ret = false;
    }
    //重采样初始化
    if(!resampleInit()){
        qDebug()<<"resampleInit  failed!" ;
        ret = false;
    }
    //音频播放器初始化
    if(!m_auPlayer->open()){
        qDebug()<<"audioPlayerInit  failed!" ;
        ret = false;
    }

    return ret;

}

void AudioThread::close()
{
    DecodeThread::close();

    std::lock_guard<std::mutex> lock(m_auMutex);
    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
        m_swr_ctx = nullptr;
    }
    if (m_auPlayer) {
        m_auPlayer->close();
    }
    m_isPause = false;
}

void AudioThread::Clear()
{
    DecodeThread::clear();
    std::lock_guard<std::mutex> lock(m_auMutex);
    if (m_auPlayer)
        m_auPlayer->clear();
    pts = 0;
}

void AudioThread::setPause(bool isPause)
{
    m_isPause = isPause;
    if (m_auPlayer)
        m_auPlayer->setPause(isPause);
}



void AudioThread::run()
{
    //缓存？
    unsigned char *pcm = new unsigned char[1024*1024*10];
    while(!m_isExit){
        AVPacket *pkt = pop();
        if (!pkt)
        {
            msleep(1);
            continue;
        }
        //自动释放pkt
        bool ret = send(pkt);
        if (!ret)
        {
            continue;
        }
        // 一次send 多次recv
        while(!m_isExit){
            AVFrame * frame = recv();
            if(!frame) break;
            pts = frame->pts - m_auPlayer->getNoPlayMs();

            int size  = resample(frame,pcm);

            //播放
            if(size>0&&!m_isPause){
                //缓冲区满时等待
                while(!m_isExit && m_auPlayer->getFree()<size){
                    msleep(1);
                }
                m_auPlayer->write(pcm, size);
            }
        }
    }
    delete[] pcm;
}


bool AudioThread::resampleInit()
{
    //锁内调用
    //音频重采样
    //输入：解码出来的音频参数
    const AVChannelLayout* in_ch_layout = &m_codec_ctx->ch_layout;
    AVSampleFormat in_sample_fmt = m_codec_ctx->sample_fmt;
    int in_sample_rate = m_codec_ctx->sample_rate;
    //输出，声卡能播放的标准格式（固定写死都可以）
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO; //双声道
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16; //16位
    int out_sample_rate = 48000;

    //重采样上下文
    m_swr_ctx = swr_alloc();
    int ret = swr_alloc_set_opts2(
        &m_swr_ctx,          // 重采样上下文

        &out_ch_layout,    // 输出声道布局
        out_sample_fmt,    // 输出格式
        out_sample_rate,   // 输出采样率

        in_ch_layout,      // 输入声道布局
        in_sample_fmt,     // 输入格式
        in_sample_rate,    // 输入采样率

        0, NULL
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

int AudioThread::resample(AVFrame *indata, unsigned char *data)
{
    if (!indata || !data) {
        av_frame_free(&indata);
        return 0;
    }
    m_auMutex.lock();
    if (!m_swr_ctx) {
        m_auMutex.unlock();
        av_frame_free(&indata);
        return 0;
    }

    //输出缓冲区包装
    uint8_t *out[] =  {data,nullptr};
    int samples = swr_convert(m_swr_ctx,
                              out,                       // 输出缓冲区
                              indata->nb_samples,        // 输出样本数（最大）
                              (const uint8_t **)indata->data,  // 输入数据
                              indata->nb_samples);       // 输入样本数
    m_auMutex.unlock();
    av_frame_free(&indata);
    if (samples <= 0) return 0;

    // 计算输出字节数：样本数 * 声道数(2) * 每个样本字节数(2)
    return samples * 2 * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
}





