#ifndef PUSHER_CAPTURESOURCE_H
#define PUSHER_CAPTURESOURCE_H

/*
 * ============================ 采集源（mini-OBS 的 "source" 层） ============================
 *
 * 一、它在这条流水线里的位置
 *
 *     摄像头/麦克风 ──[本文件]──> AVFrame(yuv420p/PCM) ──> 编码线程 ──> 封装 ──> RTMP
 *
 * 二、为什么音视频分成两个独立的源（而不是一个 source 出两种帧）
 *
 *   - 每个 AVFormatContext 都不是线程安全的，音视频各一个上下文 → 各自只被一个线程碰，
 *     不需要在这层加锁（并发正确性靠"归属明确"保证，和播放器里每个线程独占一个上下文同理）；
 *   - 采集是"实时源"：视频按 30fps 到点就有帧，音频按 48kHz 连续来；分开之后各自的节奏、
 *     各自的阻塞、各自的丢帧策略互不干扰；
 *   - 音画同步不在这层做：两边都用"自己生成的单调时间戳（微秒）"从 0 开始，
 *     真正的对齐交给推流端的统一时钟（这就是为什么下面统一不用设备时间戳，见 readFrame 注释）。
 *
 * 三、设计约定（P2 做 core/platform 分层时，这一层会原样搬进 platform）
 *
 *   - 接口里只出现 FFmpeg 类型和标准库类型，不出现任何 Qt 类型，实现里才有平台差异；
 *   - readFrame() 返回的 AVFrame* 是"带引用的、调用方负责释放"（av_frame_free），
 *     与播放器 FrameQueue 的所有权契约一致（问题文档第 13 条：谁申请谁释放）；
 *   - 一切"提议"（分辨率/帧率/采样率）都只是建议，设备可能拒绝，所以 open() 之后
 *     必须用 actualXxx() 读实际协商结果，再拿它去配置编码器。
 *
 * 四、里程碑
 *
 *   [第0步] listInputDevices()：先看清系统里有哪些设备（等价于 ffmpeg -list_devices）
 *   [第1步] 视频源：打开设备 → 读帧 → 转 yuv420p → 打印实际参数（本文件已实现）
 *   [第4步] 音频源：麦克风 → PCM 帧（本文件已实现，等编码器那边接上）
 */

#include <memory>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// ============================== 视频采集 ==============================

struct VideoSourceConfig
{
    // 设备名：
    //   Windows(dshow) → "video=OBS Virtual Camera"（注意 video= 前缀，这是 dshow 的输入名格式）
    //   Linux(v4l2)    → "/dev/video0"
    std::string device;

    // 采集后端：
    //   Windows → "dshow"    Linux → "v4l2"
    //   内置合成源（不需要任何硬件）→ "testsrc"
    std::string inputFormat;

    // 下面三个是"提议"，设备不一定接受；open() 之后用 actualXxx() 取真值
    int width  = 1280;
    int height = 720;
    int fps    = 30;

    // 是否把提议的 pixel_format 也发给设备。
    // dshow 的官方文档说 pixel_format 只在"编码器是 rawvideo"时才能设，很多设备会直接拒绝，
    // 所以默认不发，让设备自己选，我们后面统一用 swscale 转成 yuv420p。
    std::string requestedPixelFormat;
};

class IVideoSource
{
public:
    virtual ~IVideoSource() = default;

    // 打开设备并完成参数协商；失败返回 false（失败原因会打到日志里）
    virtual bool open(const VideoSourceConfig& cfg) = 0;
    virtual void close() = 0;

    // 让阻塞中的 readFrame() 尽快返回 nullptr（给 stop 用）。
    // 采集设备上的 av_read_frame 会长时间阻塞，必须有这个出口（问题文档第 17 条）。
    virtual void requestStop() = 0;

    // 取一帧视频，统一转换成 yuv420p（编码器和 VideoWidget 都只认这个格式）。
    // 返回的帧带引用：调用方用 av_frame_free() 释放；返回 nullptr = 出错 / 已被 requestStop / 设备结束。
    virtual AVFrame* readFrame() = 0;

    // open() 成功后有效：设备实际协商出来的参数
    virtual int         width()  const = 0;
    virtual int         height() const = 0;
    virtual int         fps()    const = 0;
    virtual std::string pixelFormatName() const = 0;   // 设备原生格式，如 "yuyv422" / "mjpeg"
    virtual std::string describe() const = 0;          // 一行摘要，用来打日志

    // 这一帧流是否"本来就已经是编码流"（MJPEG/H264）——只有排错时关心
    virtual bool sourceWasCompressed() const = 0;
};

// 工厂：按 cfg.inputFormat 选实现（dshow / v4l2 / testsrc）。失败返回 nullptr。
std::unique_ptr<IVideoSource> createVideoSource(const VideoSourceConfig& cfg);

// ============================== 音频采集 ==============================

struct AudioSourceConfig
{
    // Windows(dshow) → "audio=麦克风 (Realtek(R) Audio)"（名字用 --list-devices 查）
    // Linux(alsa)    → "default" 或 "hw:0"
    std::string device;

    // Windows → "dshow"    Linux → "alsa"
    std::string inputFormat;

    int sampleRate = 48000;   // 提议值
    int channels   = 2;       // 提议值
};

class IAudioSource
{
public:
    virtual ~IAudioSource() = default;

    virtual bool open(const AudioSourceConfig& cfg) = 0;
    virtual void close() = 0;
    virtual void requestStop() = 0;

    // 取一帧 PCM（格式见 sampleFormatName()，常见是 s16 / fltp）。
    // 注意：这里不做重采样，交给 [第4步] 的 AAC 编码器用 swresample 转换
    //       （因为"目标格式"应该由编码器决定，采集层不该猜）。
    virtual AVFrame* readFrame() = 0;

    virtual int         sampleRate() const = 0;
    virtual int         channels()   const = 0;
    // 采样格式：枚举值给编码器配 swresample 用，名字只用于打日志
    virtual AVSampleFormat sampleFormat() const = 0;
    virtual std::string sampleFormatName() const = 0;
    virtual std::string describe() const = 0;
};

std::unique_ptr<IAudioSource> createAudioSource(const AudioSourceConfig& cfg);

// ============================== 工具函数 ==============================

// [第0步] 枚举某类输入设备（等价于命令行的 ffmpeg -list_devices true -f dshow -i dummy）。
// 返回 0 表示成功。dshow 会把音视频分开列出来，所以打印时标了类型。
int listInputDevices(const std::string& inputFormat);

// 把一帧 yuv420p 转成 RGB 并写成 PPM(P6) 文件。
// 用途：没有预览窗口时也能"看到"采集画面（任何看图软件都能打开 PPM）。
bool saveFrameAsPpm(const AVFrame* yuv420pFrame, const std::string& path);

#endif // PUSHER_CAPTURESOURCE_H
