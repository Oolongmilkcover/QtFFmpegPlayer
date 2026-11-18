#include "frameconverter.h"
#include "core/log.h"

FrameConverter::FrameConverter() {}

bool FrameConverter::ensure(int srcW, int srcH, AVPixelFormat srcFmt, int dstW, int dstH)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ensureLocked(srcW, srcH, srcFmt, dstW, dstH);
}

bool FrameConverter::ensureLocked(int srcW, int srcH, AVPixelFormat srcFmt, int dstW, int dstH)
{
    if(m_sws && srcW == m_srcW && srcH == m_srcH
        && srcFmt == m_srcFmt && dstW == m_dstW && dstH == m_dstH ){
        return true; //参数没变，复用已有的 SwsContext
    }
    resetLocked();
    m_sws = sws_getContext(srcW,srcH,srcFmt,
                           dstW,dstH,AV_PIX_FMT_YUV420P,
                           SWS_BILINEAR,nullptr,nullptr,nullptr);
    if(!m_sws) {
        //建不起来就直接说清楚是哪组参数，否则表现是“所有帧都丢、黑屏无提示”
        logDebug() << "[FrameConverter] sws_getContext failed: srcFmt=" << (int)srcFmt
                   << " src=" << srcW << "x" << srcH
                   << " dst=" << dstW << "x" << dstH;
        return false;
    }

    m_srcW = srcW;
    m_srcH = srcH;
    m_srcFmt = srcFmt;
    m_dstH = dstH;
    m_dstW = dstW;
    return true;

}

void FrameConverter::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    resetLocked();
}

void FrameConverter::resetLocked()
{
    if(m_sws){
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_srcW = 0;
    m_srcH = 0;
    m_srcFmt = AV_PIX_FMT_NONE;
    m_dstW = 0;
    m_dstH = 0;
}


AVFrame *FrameConverter::convert(const AVFrame *src)
{
    if (!src) return nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);

    /*
     * 用“这一帧的真实格式/尺寸”来建（或复用）上下文，而不是只信 open() 时从
     * codecpar 取的那一份：解码器实际输出可能和 codecpar 不一致
     * （yuvj420p / nv12 / 10bit / 硬解），按错误布局喂 sws_scale 会花屏甚至越界读。
     * ensureLocked() 在参数没变时直接返回，所以每帧调用没有额外开销。
     * 输出尺寸固定等于输入尺寸 —— 上屏那层要求帧尺寸和视频原始宽高一致。
     */
    if (!ensureLocked(src->width, src->height, (AVPixelFormat)src->format,
                      src->width, src->height)) {
        return nullptr;
    }

    // 源本来就是"同尺寸 + yuv420p + limited range"：直接加一次引用返回（零拷贝）
    // [重点] 为什么要判 color_range：yuvj* 那一族的 420p 名字一样但语义是 full range，
    //        走这条分支会让画面发灰，宁可让 swscale 老老实实转一次。
    if (src->format == AV_PIX_FMT_YUV420P
        && src->color_range == AVCOL_RANGE_MPEG) {
        AVFrame* same = av_frame_alloc();
        if (!same) return nullptr;
        if (av_frame_ref(same, src) < 0) { av_frame_free(&same); return nullptr; }
        return same;      // av_frame_ref 会连 pts/duration 一起带过去
    }

    AVFrame* dst = av_frame_alloc();
    if(!dst) return nullptr;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = m_dstW;
    dst->height = m_dstH;

    if(av_frame_get_buffer(dst,32)<0){
        av_frame_free(&dst);
        return nullptr;
    }
    const int scaled = sws_scale(m_sws,src->data,src->linesize,0,src->height,
                                 dst->data,dst->linesize);
    if (scaled <= 0) {
        logDebug() << "[FrameConverter] sws_scale failed, fmt=" << (int)src->format
                   << " size=" << src->width << "x" << src->height;
        av_frame_free(&dst);
        return nullptr;
    }
    dst->pts      = src->pts;
    dst->duration = src->duration;
    return dst;
}



FrameConverter::~FrameConverter()
{
    reset();
}
