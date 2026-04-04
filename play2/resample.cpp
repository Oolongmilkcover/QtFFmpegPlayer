#include "resample.h"
#include <iostream>
using namespace std;

void Resample::Close()
{
    mux.lock();
    if (actx)
        swr_free(&actx);

    mux.unlock();
}

//输出参数和输入参数一致除了采样格式，输出为S16
bool Resample::Open(AVCodecParameters *para,bool isClearPara)
{
    if (!para) return false;
    Close();

    // 1. 获取并校验音频参数
    int sampleRate = para->sample_rate;
    if (sampleRate <= 0) {
        cerr << "Resample: invalid sample rate, use 48000" << endl;
        sampleRate = 48000;
    }

    // 输入采样格式
    AVSampleFormat inFormat = static_cast<AVSampleFormat>(para->format);
    if (inFormat < 0 || inFormat >= AV_SAMPLE_FMT_NB) {
        cerr << "Resample: invalid sample format, use AV_SAMPLE_FMT_FLTP" << endl;
        inFormat = AV_SAMPLE_FMT_FLTP;
    }

    // 输入通道布局
    AVChannelLayout inLayout;
    if (av_channel_layout_check(&para->ch_layout) && para->ch_layout.nb_channels > 0) {
        // 深拷贝输入布局
        av_channel_layout_copy(&inLayout, &para->ch_layout);
    } else {
        // 默认立体声
        cerr << "Resample: invalid channel layout, set to stereo" << endl;
        av_channel_layout_default(&inLayout, 2);
    }

    // 输出布局：立体声 (2 channels)
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 2);

    // 2. 分配并配置重采样上下文
    mux.lock();

    SwrContext *ctx = nullptr;
    int ret = swr_alloc_set_opts2(&ctx,
                                  &outLayout,                // 输出声道布局
                                  outFormat,                 // 输出采样格式 (S16)
                                  sampleRate,                // 输出采样率
                                  &inLayout,                 // 输入声道布局
                                  inFormat,                  // 输入采样格式
                                  sampleRate,                // 输入采样率
                                  0, nullptr);               // 日志参数
    if (ret < 0 || !ctx) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        cerr << "swr_alloc_set_opts2 failed: " << errbuf << endl;
        mux.unlock();
        if (isClearPara) avcodec_parameters_free(&para);
        return false;
    }

    // 3. 初始化重采样器
    ret = swr_init(ctx);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        cerr << "swr_init failed: " << errbuf << endl;
        swr_free(&ctx);
        mux.unlock();
        if (isClearPara) avcodec_parameters_free(&para);
        return false;
    }

    actx = ctx;
    mux.unlock();

    if (isClearPara) avcodec_parameters_free(&para);
    cout << "Resample initialized successfully" << endl;
    return true;
}

//返回重采样后大小,不管成功与否都释放indata空间
int Resample::Resampledata(AVFrame *indata, unsigned char *data)
{
    if (!indata || !data) {
        av_frame_free(&indata);
        return 0;
    }

    mux.lock();
    if (!actx) {
        mux.unlock();
        av_frame_free(&indata);
        return 0;
    }

    // 输出缓冲区包装
    uint8_t *out[] = { data, nullptr };
    int samples = swr_convert(actx,
                              out,                       // 输出缓冲区
                              indata->nb_samples,        // 输出样本数（最大）
                              (const uint8_t **)indata->data,  // 输入数据
                              indata->nb_samples);       // 输入样本数

    mux.unlock();
    av_frame_free(&indata);

    if (samples <= 0) return 0;

    // 计算输出字节数：样本数 * 声道数(2) * 每个样本字节数(2)
    return samples * 2 * av_get_bytes_per_sample(outFormat);
}

Resample::Resample()
{
}


Resample::~Resample()
{
}
