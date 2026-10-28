#ifndef PUSHER_ENCODER_H
#define PUSHER_ENCODER_H

/*
 * 编码器（mini-OBS 的 "encoder" 层）
 *
 * 约定：
 *   - 输入是 yuv420p 的 AVFrame（视频）/ 已对齐的 PCM AVFrame（音频）；
 *   - 输出通过 PacketSink 回调交给上层（上层负责放进有界队列）；
 *     回调里拿到的 AVPacket 是"借用"的，回调内不要保存指针；
 *   - 编码失败/被丢帧都返回 false，不抛异常（实时链路里不允许异常）。
 *
 * 里程碑对应：
 *   [第2步] libx264：采集 → 编码 → 落盘 mp4，用 ffprobe 和 ffmpeg CLI 对照
 *   [第4步] AAC：麦克风 → 编码 → 与视频同一个容器
 */
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

struct EncoderConfig
{
    std::string videoCodec = "libx264";
    std::string audioCodec = "aac";

    int width  = 1280;
    int height = 720;
    int fps    = 30;

    int videoBitrate = 4000000;
    int audioBitrate = 128000;

    // 低延迟相关的三个旋钮（[第5步] 参数实验就从这里改）
    int  gopSize      = 30;         // = fps → 1 秒一个关键帧
    int  maxBFrames   = 0;          // 推流先给 0（B 帧会带来 dts<pts 和额外缓冲）
    bool zerolatency  = true;       // tune=zerolatency
    bool cbr          = false;      // 码率模式：false=普通 VBR（x264 默认），true=钉住码率（接近 CBR）
    std::string preset = "veryfast";

    // 喂进来的 AVFrame 的 pts 用的是哪个时间基。
    // 采集源统一产出"微秒"，所以默认 {1,1000000}；编码时会换算到编码器自己的 time_base。
    AVRational inputTimeBase{ 1, 1000000 };

    // ---- 音频（[第4步]）----
    // 这三个描述"麦克风实际给出来的 PCM"格式，由 Pusher 打开音频源之后回填（用实际值，不用提议值）。
    // AAC 编码器内部会用 swresample 把它转成自己要的格式（AAC 只吃 fltp），所以这里允许任意输入。
    int             audioInSampleRate = 48000;
    int             audioInChannels   = 2;
    AVSampleFormat  audioInSampleFmt  = AV_SAMPLE_FMT_S16;
};

// 编码器产出一包码流时回调（只在这一刻借用指针）
using PacketSink = std::function<void(AVPacket*)>;

class IVideoEncoder
{
public:
    virtual ~IVideoEncoder() = default;

    virtual bool open(const EncoderConfig& cfg) = 0;
    virtual void setSink(PacketSink sink) = 0;

    // 喂一帧 yuv420p；返回 false 表示这一帧没有被接受（丢弃或出错）
    virtual bool encode(AVFrame* yuv420p) = 0;

    // 收尾：把编码器内部还压着的包全部吐出来（推流结束时必须调）
    virtual void flush() = 0;

    // 给 muxer 建流用的参数（avcodec_parameters_from_context 的结果）
    virtual AVCodecParameters* codecpar() = 0;

    // 编码器的时间基（通常 = {1,fps}）。muxer 建流、写包时都要用它做时间戳换算，
    // 所以由编码器自己报出来，别让上层去猜（换成硬件编码器时它可能不一样）。
    virtual AVRational timeBase() const = 0;
};

class IAudioEncoder
{
public:
    virtual ~IAudioEncoder() = default;

    // cfg 里的 audioInSampleRate/audioInChannels/audioInSampleFmt 描述"喂进来的 PCM"是什么格式；
    // 编码器内部自己用 swresample 转成它需要的格式（AAC 只吃 fltp 平面浮点）。
    virtual bool open(const EncoderConfig& cfg) = 0;
    virtual void setSink(PacketSink sink) = 0;
    virtual bool encode(AVFrame* pcm) = 0;
    virtual void flush() = 0;
    virtual AVCodecParameters* codecpar() = 0;
    virtual AVRational timeBase() const = 0;      // 音频通常是 {1, sample_rate}
};

std::unique_ptr<IVideoEncoder> createVideoEncoder(const EncoderConfig& cfg);
std::unique_ptr<IAudioEncoder> createAudioEncoder(const EncoderConfig& cfg);

#endif // PUSHER_ENCODER_H
