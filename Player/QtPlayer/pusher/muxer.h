#ifndef PUSHER_MUXER_H
#define PUSHER_MUXER_H

/*
 * 封装 / 输出（mini-OBS 的 "output" 层）
 *
 * 同一个实现既支持推流也支持落盘，只差 OutputConfig：
 *   rtmp://host/live/key   + format="flv"   → 推流（RTMP）
 *   out.mp4                + format="mp4"   → 落盘（验证编码参数用）
 *
 * 约定：
 *   - writePacket() 可能在网络上阻塞 → 只在推流线程里调用；
 *   - 网络路径必须设 rw_timeout + 中断回调，否则关窗口/断网会卡死（问题文档第 17 条）。
 *
 * 里程碑对应：
 *   [第2步] mp4 落盘（先用它验证时间基/编码参数，再谈推流）
 *   [第3步] flv + rtmp 推给 SRS
 */
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

struct OutputConfig
{
    std::string url;              // "rtmp://192.168.1.50/live/demo" 或 "out.mp4"
    std::string format;           // "flv" / "mp4"；留空则按 url 的扩展名/协议猜
    int  rwTimeoutMs = 5000;      // 网络读写超时（推流必须设）
    bool reconnect   = true;      // 断线重连（[第5步] 演示用）
};

class IMuxer
{
public:
    virtual ~IMuxer() = default;

    virtual bool open(const OutputConfig& cfg) = 0;

    // 视频/音频各调一次。
    // timeBase 是送进来的包用的时间基（= 编码器的 time_base）：muxer 要记住它，
    // 才能在 writePacket 里把时间戳换算到输出流自己的时间基上
    // （FLV 是毫秒；mp4 常被 muxer 改写成 1/90000 之类 —— 所以必须"现查现换算"）。
    virtual AVStream* addStream(AVCodecParameters* par, AVRational timeBase) = 0;

    virtual bool writeHeader() = 0;

    // 写一包（内部按 dts 交织）；可能阻塞 → 在推流线程调用。
    // 注意：av_interleaved_write_frame 会接管并 unref 这个包，调用方别再多做一次 unref。
    virtual bool writePacket(AVPacket* pkt) = 0;

    virtual void writeTrailer() = 0;

    // 输出是否还活着。写失败/掉线之后为 false，推流线程据此触发重连。
    virtual bool isOpen() const = 0;

    // 叫停正在阻塞的网络写（关程序时用；rw_timeout 是第二道保险）。
    // 只设置一个原子标志，由 AVIOInterruptCB 在读它，所以从别的线程调是安全的。
    virtual void abort() = 0;
};

std::unique_ptr<IMuxer> createMuxer(const OutputConfig& cfg);

#endif // PUSHER_MUXER_H
