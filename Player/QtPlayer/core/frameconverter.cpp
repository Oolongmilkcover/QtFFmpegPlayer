#include "frameconverter.h"
FrameConverter::FrameConverter() {}

bool FrameConverter::ensure(int srcW, int srcH, AVPixelFormat srcFmt, int dstW, int dstH)
{
    if(m_sws && srcW == m_srcW && srcH == m_srcH
        && srcFmt == m_srcFmt && dstW == m_dstW && dstH == m_dstH ){
        return true; //参数没变，复用已有的 SwsContext
    }
    reset();
    m_sws = sws_getContext(srcW,srcH,srcFmt,
                           dstW,dstH,AV_PIX_FMT_YUV420P,
                           SWS_BILINEAR,nullptr,nullptr,nullptr);
    if(!m_sws) return false;

    m_srcW = srcW;
    m_srcH = srcH;
    m_srcFmt = srcFmt;
    m_dstH = dstH;
    m_dstW = dstW;
    return true;

}

void FrameConverter::reset()
{
    if(m_sws){
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
}


AVFrame *FrameConverter::convert(const AVFrame *src)
{
    if (!m_sws || !src) return nullptr;

    // 源本来就是"同尺寸 + yuv420p + limited range"：直接加一次引用返回（零拷贝）
    // [重点] 为什么要判 color_range：yuvj* 那一族的 420p 名字一样但语义是 full range，
    //        走这条分支会让画面发灰，宁可让 swscale 老老实实转一次。
    if (src->format == AV_PIX_FMT_YUV420P
        && src->width == m_srcW && src->height == m_srcH
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
    sws_scale(m_sws,src->data,src->linesize,0,src->height,dst->data,dst->linesize);
    dst->pts      = src->pts;
    dst->duration = src->duration;
    return dst;
}



FrameConverter::~FrameConverter()
{
    reset();
}
