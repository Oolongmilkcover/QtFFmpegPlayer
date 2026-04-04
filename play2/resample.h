
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include "libswresample/swresample.h"
}
#include <mutex>
class Resample
{
public:

    //输出参数和输入参数一致除了采样格式，输出为S16 ,会释放para
    virtual bool Open(AVCodecParameters *para,bool isClearPara = false);
    virtual void Close();

    //返回重采样后大小,不管成功与否都释放indata空间
    virtual int Resampledata(AVFrame *indata, unsigned char *data);
    Resample();
    ~Resample();

    //AV_SAMPLE_FMT_S16
    AVSampleFormat outFormat = AV_SAMPLE_FMT_S16;
protected:
    std::mutex mux;
    SwrContext *actx = nullptr;
};

