#ifndef FRAMECONVERTER_H
#define FRAMECONVERTER_H

#include <mutex>

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
    // 内部实现：调用者必须已持有 m_mutex（避免公开接口和 convert 里互相递归加锁）
    bool ensureLocked(int srcW,int srcH,AVPixelFormat srcFmt,int dstW,int dstH);
    void resetLocked();

    /*
     * 同一个转换器会被两条线程用到：解码线程 run() 和 demux 线程的 repaintPts()。
     * sws_scale / SwsContext 不是线程安全的，所以锁放在这里，
     * 不依赖调用方“恰好串行”这个假设。
     */
    std::mutex m_mutex;

    SwsContext* m_sws = nullptr;
    int m_srcW  = 0;
    int m_srcH  = 0;
    AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
    int m_dstW = 0;
    int m_dstH = 0;
};

#endif // FRAMECONVERTER_H
