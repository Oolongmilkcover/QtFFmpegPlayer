#ifndef FRAMECONVERTER_H
#define FRAMECONVERTER_H
extern "C"{
    #include <libswscale/swscale.h>
}
//统一转换为YUV420p
class FrameConverter
{
public:
    FrameConverter();
    ~FrameConverter();

    // 源格式/尺寸变了就重建上下文（设备中途改格式的情况确实存在）
    bool ensure(int srcW,int srcH,AVPixelFormat srcFmt , int dstW,int dstH);

    void reset();

    // 转成新的 yuv420p 帧（带引用，调用方 av_frame_free）。pts 由调用方设置
    AVFrame* convert(const AVFrame* src);
private:
    SwsContext* m_sws = nullptr;
    int m_srcW  = 0;
    int m_srcH  = 0;
    AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
    int m_dstW = 0;
    int m_dstH = 0;
};

#endif // FRAMECONVERTER_H
