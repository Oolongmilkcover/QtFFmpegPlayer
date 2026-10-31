#include "capturesource.h"

#include "log.h"

extern "C" {
#include <libavdevice/avdevice.h>      // dshow / v4l2 / alsa 这些"输入设备"都在这个库里
#include <libavformat/avformat.h>      // avformat_open_input / av_read_frame
#include <libavcodec/avcodec.h>        // 解码器（设备给 MJPEG/H264 时要自己解码）
#include <libswscale/swscale.h>        // 任意像素格式 → yuv420p；以及 YUV → RGB 存 PPM
#include <libavutil/imgutils.h>        // av_image_* 系列
#include <libavutil/opt.h>             // av_opt_set（编码器/设备的私有选项）
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// ============================================================================================
// 小工具
// ============================================================================================

// FFmpeg 的错误码要自己转字符串。
// 注意：C++ 里不能用 av_err2str()（它用了 C 的复合字面量，编译不过），必须用 av_strerror。
static std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

static std::string pixName(int fmt)
{
    const char* n = av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt));
    return n ? n : "unknown";
}

static std::string sampleName(int fmt)
{
    const char* n = av_get_sample_fmt_name(static_cast<AVSampleFormat>(fmt));
    return n ? n : "unknown";
}

/*
 * FFmpeg 的中断回调。
 *
 * 为什么必须有它：采集设备上的 avformat_open_input / av_read_frame 会长时间阻塞
 * （等设备出帧）。用户按 Ctrl+C 或程序要退出时，如果线程卡在阻塞调用里，
 * 就只能强杀进程——文件写不出 trailer、网络来不及告别。
 * 注册这个回调后，FFmpeg 会在内部轮询它，返回非 0 就立刻放弃当前 I/O（返回 AVERROR_EXIT）。
 *
 * 这正是问题文档第 17 条那套思路，只不过那边等的是网络流，这里等的是设备。
 */
static int interruptCb(void* opaque)
{
    auto* stop = static_cast<std::atomic<bool>*>(opaque);
    return stop->load() ? 1 : 0;
}

/*
 * swscale 封装：把设备给的"任意像素格式 + 任意尺寸"统一转成 yuv420p。
 *
 * 为什么一定要转：你的 VideoWidget 按三平面 GL_RED 上传（只认 yuv420p），
 * x264 编码器也要求 yuv420p（或 nv12 等少数格式）。而设备给什么五花八门：
 *   - YUY2 / NV12：原始格式，直接转；
 *   - MJPEG / H264：先解码成 AVFrame，再转。
 */
class FrameConverter
{
public:
    ~FrameConverter() { close(); }

    bool init(int srcW, int srcH, AVPixelFormat srcFmt, int dstW, int dstH)
    {
        close();
        // 注意：dstW/dstH 这里和源相同（只做格式转换，不做缩放）；
        // 以后要"设备 4K 但推 1080p"，改这两个参数就行，代码不用动。
        m_sws = sws_getContext(srcW, srcH, srcFmt,
                               dstW, dstH, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            logWarn() << "[capture] sws_getContext 失败:" << srcW << "x" << srcH
                       << pixName(srcFmt).c_str() << "->" << dstW << "x" << dstH << "yuv420p";
            return false;
        }
        m_w = dstW;
        m_h = dstH;
        return true;
    }

    void close()
    {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    }

    // 把 src 转成一张新的 yuv420p 帧返回（带引用，调用方 av_frame_free）；
    // pts 由调用方设置（这里只负责像素）。
    AVFrame* convert(const AVFrame* src)
    {
        if (!m_sws || !src) return nullptr;

        AVFrame* dst = av_frame_alloc();
        if (!dst) return nullptr;
        dst->format = AV_PIX_FMT_YUV420P;
        dst->width  = m_w;
        dst->height = m_h;

        // av_frame_get_buffer 会按格式算好每个平面需要多少字节并分配（比手算 Y/U/V 大小安全）。
        // 第二个参数是对齐字节数：给 GPU / SIMD 用 32 比较稳。
        int ret = av_frame_get_buffer(dst, 32);
        if (ret < 0) {
            logWarn() << "[capture] av_frame_get_buffer 失败:" << errStr(ret).c_str();
            av_frame_free(&dst);
            return nullptr;
        }

        // sws_scale 只搬像素：src 的平面/行宽 → dst 的平面/行宽（行对齐由 FFmpeg 处理）
        sws_scale(m_sws,
                  src->data, src->linesize, 0, src->height,
                  dst->data, dst->linesize);
        return dst;
    }

private:
    SwsContext* m_sws = nullptr;
    int m_w = 0, m_h = 0;
};

// ============================================================================================
// 视频源一：真实设备（Windows: dshow，Linux: v4l2）
// ============================================================================================

/*
 * 整个流程和你播放器里的 demux + decode 是同一套，只是把"输入源"从文件换成了设备：
 *
 *   1) av_find_input_format("dshow")        找到"设备这种输入格式"（文件不需要这一步）
 *   2) avformat_alloc_context() + 中断回调   让阻塞能被中断
 *   3) AVDictionary 提参数                   分辨率/帧率/像素格式（只是建议！）
 *   4) avformat_open_input()                 打开设备
 *   5) avformat_find_stream_info()           让 FFmpeg 读一点数据，把真实参数填进 stream
 *   6) av_find_best_stream(VIDEO)            找视频流（顺手拿到该流的解码器）
 *   7) avcodec_open2()                       开解码器（MJPEG/H264 必须；rawvideo 也走同一条路）
 *   8) sws_getContext()                      准备"设备格式 → yuv420p"的转换器
 *   之后就是循环 av_read_frame → 解码 → swscale → 交出去。
 */
class DeviceVideoSource : public IVideoSource
{
public:
    ~DeviceVideoSource() override { close(); }

    bool open(const VideoSourceConfig& cfg) override
    {
        close();
        m_cfg = cfg;

        // ---- 1) 找到输入格式 ----
        const AVInputFormat* ifmt = av_find_input_format(cfg.inputFormat.c_str());
        if (!ifmt) {
            logWarn() << "[capture] 找不到输入格式" << cfg.inputFormat.c_str()
                       << "；这份 FFmpeg 可能没编进该设备（可以用 --list-devices 或"
                          " 命令行 ffmpeg -devices 确认）";
            return false;
        }

        // ---- 2) 先建 context 并挂中断回调，再打开 ----
        // 顺序很重要：回调必须在 avformat_open_input 之前挂上，
        // 否则"打开设备"这一步卡住时同样没法中断。
        m_fmt = avformat_alloc_context();
        if (!m_fmt) return false;
        m_fmt->interrupt_callback.callback = &interruptCb;
        m_fmt->interrupt_callback.opaque   = &m_stop;

        // ---- 3) 参数用 AVDictionary 提（键名见 FFmpeg 官方 Devices 文档）----
        AVDictionary* opts = nullptr;
        const std::string size = std::to_string(cfg.width) + "x" + std::to_string(cfg.height);
        av_dict_set(&opts, "video_size", size.c_str(), 0);            // 分辨率
        av_dict_set(&opts, "framerate",  std::to_string(cfg.fps).c_str(), 0);  // 帧率
        if (!cfg.requestedPixelFormat.empty()) {
            // dshow 的 pixel_format 只在"编码器为 rawvideo"时允许设置，很多设备会直接拒绝，
            // 所以默认不传（见 VideoSourceConfig 的注释）。
            av_dict_set(&opts, "pixel_format", cfg.requestedPixelFormat.c_str(), 0);
        }

        // ---- 4) 打开设备 ----
        int ret = avformat_open_input(&m_fmt, cfg.device.c_str(), ifmt, &opts);
        if (ret < 0) {
            logWarn() << "[capture] 打不开设备" << cfg.device.c_str()
                       << ":" << errStr(ret).c_str();
            if (opts) av_dict_free(&opts);
            // avformat_open_input 失败时自己已经把 m_fmt 释放了，这里只要清指针
            m_fmt = nullptr;
            return false;
        }
        // 设备不一定接受上面所有建议：把没被吃掉的选项打出来，方便排查
        if (opts) {
            AVDictionaryEntry* e = nullptr;
            while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) != nullptr)
                logWarn() << "[capture] 设备忽略了参数:" << e->key << "=" << e->value;
            av_dict_free(&opts);
        }

        // ---- 5) 读一点数据，把真实参数填进 stream ----
        // 这一步之后 codecpar 里的 width/height/format/framerate 才是设备实际给的值。
        ret = avformat_find_stream_info(m_fmt, nullptr);
        if (ret < 0) {
            logWarn() << "[capture] avformat_find_stream_info 失败:" << errStr(ret).c_str();
            return false;
        }

        // ---- 6) 找视频流 ----
        // av_find_best_stream 会返回流下标，并通过最后一个参数把"推荐解码器"给你。
        const AVCodec* decoder = nullptr;
        m_videoStreamIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (m_videoStreamIndex < 0 || !decoder) {
            logWarn() << "[capture] 这个设备没有视频流:" << cfg.device.c_str();
            return false;
        }
        AVStream* st = m_fmt->streams[m_videoStreamIndex];
        AVCodecParameters* par = st->codecpar;

        m_width  = par->width;
        m_height = par->height;
        m_devicePixFmt = static_cast<AVPixelFormat>(par->format);
        // 帧率：设备可能给 30/1 也可能给 30000/1001，取近似整数
        m_fps = (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
                    ? av_rescale_q(1, AVRational{ st->avg_frame_rate.den, st->avg_frame_rate.num },
                                   AVRational{ 1, 1 })
                    : 0;
        if (m_fps <= 0) m_fps = cfg.fps > 0 ? cfg.fps : 30;

        // ---- 7) 开解码器 ----
        // 关键认知：设备给的不一定是原始帧。USB 摄像头默认常常是 MJPEG（压缩的），
        // 这时必须像播放器一样先解码；即使是 YUY2/NV12 这种原始格式，
        // 统一走解码器（rawvideo 也有解码器）能让代码只有一条路径。
        m_decCtx = avcodec_alloc_context3(decoder);
        if (!m_decCtx) return false;
        ret = avcodec_parameters_to_context(m_decCtx, par);
        if (ret < 0) { logWarn() << "[capture] avcodec_parameters_to_context 失败:" << errStr(ret).c_str(); return false; }
        m_decCtx->pkt_timebase = st->time_base;   // 有些解码器需要它来算 pts
        // 采集是实时的，多线程解码意义不大（而且会增加一帧延迟），保持默认即可
        ret = avcodec_open2(m_decCtx, decoder, nullptr);
        if (ret < 0) { logWarn() << "[capture] avcodec_open2 失败:" << errStr(ret).c_str(); return false; }

        // ---- 8) 准备格式转换器 ----
        if (!m_converter.init(m_width, m_height, static_cast<AVPixelFormat>(m_decCtx->pix_fmt),
                              m_width, m_height)) {
            return false;
        }

        // 解码后的帧用这个复用（每次 receive_frame 前先 unref，避免频繁分配）
        m_decodedFrame = av_frame_alloc();
        m_packet       = av_packet_alloc();
        if (!m_decodedFrame || !m_packet) return false;

        // 打印"实际"参数——这是 [第1步] 最重要的一行日志
        logInfo().nospace() << "[capture] 设备打开成功: " << cfg.device.c_str()
                          << "  实际 " << m_width << "x" << m_height
                          << " @" << m_fps
                          << "  设备原生格式=" << pixName(m_devicePixFmt).c_str()
                          << "  解码后格式=" << pixName(m_decCtx->pix_fmt).c_str()
                          << "  流时间基=" << st->time_base.num << "/" << st->time_base.den;
        return true;
    }

    void close() override
    {
        if (m_packet)       av_packet_free(&m_packet);
        if (m_decodedFrame) av_frame_free(&m_decodedFrame);
        m_converter.close();
        if (m_decCtx)       avcodec_free_context(&m_decCtx);
        if (m_fmt)          avformat_close_input(&m_fmt);   // 它会顺便 free context
        m_videoStreamIndex = -1;
    }

    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {
        if (!m_fmt) return nullptr;

        while (true) {
            if (m_stop.load()) return nullptr;

            // 读一包：这一步在设备上会阻塞，直到设备出一帧（或中断回调让它返回）
            int ret = av_read_frame(m_fmt, m_packet);
            if (ret < 0) {
                if (ret != AVERROR_EXIT)     // AVERROR_EXIT 就是被我们自己的中断回调打断的
                    logWarn() << "[capture] av_read_frame 结束:" << errStr(ret).c_str();
                return nullptr;
            }

            // 只关心视频流：如果是同一个 input 里的音频包（dshow 允许 video+audio 放一起），直接丢弃
            if (m_packet->stream_index != m_videoStreamIndex) {
                av_packet_unref(m_packet);
                continue;
            }

            // ---- 送进解码器 ----
            ret = avcodec_send_packet(m_decCtx, m_packet);
            av_packet_unref(m_packet);          // 送完就能还包了（解码器内部已引用它需要的数据）
            if (ret < 0) {
                logWarn() << "[capture] avcodec_send_packet 失败:" << errStr(ret).c_str();
                continue;                       // 单包出错不要退出，继续抓下一帧
            }

            // ---- 取解码结果 ----
            // 一个包可能出 0 帧（还没凑够）也可能出多帧（B 帧重排），所以要循环。
            while (true) {
                ret = avcodec_receive_frame(m_decCtx, m_decodedFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;   // 暂时没有更多帧
                if (ret < 0) {
                    logWarn() << "[capture] avcodec_receive_frame 失败:" << errStr(ret).c_str();
                    break;
                }

                // ---- 转成 yuv420p 并交出去 ----
                AVFrame* out = m_converter.convert(m_decodedFrame);
                av_frame_unref(m_decodedFrame);   // 用完了，立刻还引用（解码器可以复用这块内存）
                if (!out) return nullptr;

                // ---- 时间戳：自己生成，不用设备给的 ----
                // 原因：不同后端差异很大（v4l2 是 1 微秒单位、dshow 常常是 100ns；
                //      FFmpeg 官方还专门给了 use_video_device_timestamps 开关来绕开"设备时间戳不可靠"）。
                //
                // 这里用单调时钟，而不是"帧号 × 帧周期"，这一点很关键：
                //   设备实际帧率常常是 29.97 之类，若按 30 算，视频时间轴就比真实时间快，
                //   而音频那边是按真实采样数走的 —— 推半小时会积累出秒级音画漂移。
                //   用墙上时间做 pts，视频时间轴就等于真实时间，和音频天然一致。
                // 合成源不需要这么做：它本来就按帧率节拍生成，帧号就等于真实时间。
                static const auto clockBase = std::chrono::steady_clock::now();
                const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - clockBase).count();

                // ---- 最高帧率限制（丢帧式）----
                // 真实摄像头是按请求帧率出帧的，不需要我们管；但 OBS / AMD Privacy View 这类
                // 虚拟摄像头可能"全速出帧"（实测能到几百 fps），不限制就会把编码器灌爆、
                // 文件里塞进几百 fps 的码流。
                // 为什么用"丢"而不是"sleep"：sleep 会让驱动的帧队列积压、延迟越积越大；
                // 丢掉过早的帧，能保证我们处理的永远是"最新"的那一帧（这也和 pts 用墙上时间自洽）。
                const int64_t periodUs = 1000000 / (m_fps > 0 ? m_fps : 30);
                if (m_lastFrameUs >= 0 && nowUs - m_lastFrameUs < periodUs * 9 / 10) {
                    ++m_droppedByRateLimit;
                    av_frame_free(&out);      // 这一帧不要了
                    continue;
                }
                m_lastFrameUs = nowUs;

                out->pts = nowUs;
                out->duration = periodUs;
                return out;   // 引用交给调用方
            }
        }
    }

    int         width()  const override { return m_width; }
    int         height() const override { return m_height; }
    int         fps()    const override { return m_fps; }
    //返回设备原始输出的像素格式名称
    std::string pixelFormatName() const override { return pixName(m_devicePixFmt); }
    //判断“采集到的数据是不是经过解码的”
    bool        sourceWasCompressed() const override
    {
        // rawvideo = 设备直接给原始帧；其它（mjpeg/h264…）= 设备给的是压缩流，我们解码了
        return m_decCtx && m_decCtx->codec_id != AV_CODEC_ID_RAWVIDEO;
    }

    std::string describe() const override
    {
        return std::to_string(m_width) + "x" + std::to_string(m_height)
             + "@" + std::to_string(m_fps)
             + " 设备格式=" + pixName(m_devicePixFmt)
             + (sourceWasCompressed() ? " (已解码)" : " (原始帧)");
    }

private:
    VideoSourceConfig m_cfg;
    AVFormatContext*  m_fmt = nullptr;
    AVCodecContext*   m_decCtx = nullptr;
    AVFrame*          m_decodedFrame = nullptr;
    AVPacket*         m_packet = nullptr;
    FrameConverter    m_converter;
    int               m_videoStreamIndex = -1;
    int               m_width = 0, m_height = 0, m_fps = 0;
    AVPixelFormat     m_devicePixFmt = AV_PIX_FMT_NONE;
    std::atomic<bool> m_stop{ false };
    // 最高帧率限制用：上一帧交出去的时刻（微秒）与因此丢掉的帧数
    int64_t           m_lastFrameUs = -1;
    int64_t           m_droppedByRateLimit = 0;
};

// ============================================================================================
// 视频源二：内置合成源（不需要任何硬件，"testsrc"）
// ============================================================================================

/*
 * 为什么要有它：
 *   - 你现在只有麦克风、没有摄像头，但想立刻验证"采集 → 编码 → 推流"这条链；
 *   - CI（无桌面、无设备）也需要一个能跑的输入源；
 *   - 调参实验（码率/GOP/延迟）用合成源比用摄像头更可控。
 *
 * 它按帧率节流（sleep_until），所以行为跟真实设备一样是"实时"的，
 * 不会一秒钟灌进去几万帧把编码器打爆。
 */
class SyntheticVideoSource : public IVideoSource
{
public:
    ~SyntheticVideoSource() override { close(); }

    bool open(const VideoSourceConfig& cfg) override
    {
        close();
        m_cfg   = cfg;
        m_width = cfg.width  > 0 ? cfg.width  : 1280;
        m_height= cfg.height > 0 ? cfg.height : 720;
        // yuv420p 与 x264 都要求宽高是偶数；奇数会在 avcodec_open2 里报
        // "Generic error in an external library"（完全看不出原因），所以这里直接向上取偶
        if (m_width % 2)  { logWarn() << "[capture] 宽度" << m_width  << "是奇数，已向上取偶"; ++m_width;  }
        if (m_height % 2) { logWarn() << "[capture] 高度" << m_height << "是奇数，已向上取偶"; ++m_height; }
        m_fps   = cfg.fps    > 0 ? cfg.fps    : 30;
        m_frame = av_frame_alloc();
        if (!m_frame) return false;
        m_frame->format = AV_PIX_FMT_YUV420P;
        m_frame->width  = m_width;
        m_frame->height = m_height;
        if (av_frame_get_buffer(m_frame, 32) < 0) return false;

        m_startTime = std::chrono::steady_clock::now();
        logInfo().nospace() << "[capture] 合成源(testsrc) 启动: " << m_width << "x" << m_height
                          << "@" << m_fps << " yuv420p（不需要任何硬件）";
        return true;
    }

    void close() override
    {
        if (m_frame) av_frame_free(&m_frame);
    }

    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {
        if (!m_frame) return nullptr;

        // ---- 按帧率节流：等到"这一帧应该出现的时刻" ----
        // 用绝对时刻（start + n*周期）而不是 sleep(周期)，
        // 这样即使某一帧处理超时，也不会把误差累积下去（不会越跑越慢）。
        const auto period = std::chrono::microseconds(1000000 / (m_fps > 0 ? m_fps : 30));
        const auto deadline = m_startTime + period * m_frameIndex;
        std::this_thread::sleep_until(deadline);
        if (m_stop.load()) return nullptr;

        // ---- 生成画面 ----
        // 刻意保持简单（不画文字，避免引入字体库依赖）：
        //   Y 平面 = 深灰底 + 一条横向滚动的亮竖条 + 一条纵向滚动的亮横条
        //   U/V 平面 = 128（中性色）→ 整体是灰阶画面
        // 这样能一眼看出：有没有出帧、运动方向对不对、帧率稳不稳。

        /*
        av_frame_make_writable
        检查 m_frame 的底层 buffer 引用计数。
        如果引用计数为 1：说明只有 m_frame 自己用这块内存。直接返回，后面你画图就原地修改，不会影响任何人。
        如果引用计数大于 1（比如现在变成了 2，因为 out 也在引用它）：
        它会重新分配一块新的缓冲区给 m_frame。
        把旧数据拷贝到新缓冲区（或者直接让 m_frame 指向新缓冲区，旧缓冲区的引用计数减 1）。
        然后返回。此时 m_frame 指向全新的内存，而 out 仍然指向旧的、属于它自己的内存。
        这就是写时复制（Copy-On-Write）。
        */
        if (av_frame_make_writable(m_frame) < 0) return nullptr;

        const int barW = m_width / 8;                       // 竖条宽度
        const int barH = m_height / 8;                      // 横条高度
        // 从 0 开始扫（这样第 0 帧也看得见），按帧号平移
        const int barX = static_cast<int>((m_frameIndex * 7) % (m_width + barW)) - barW / 2;
        const int barY = static_cast<int>((m_frameIndex * 5) % (m_height + barH)) - barH / 2;

        // 中心固定十字（约 4px 宽）：保证任何一帧都有可辨认图案 ——
        // 尤其是用 --dump-frame 落盘验收的那一帧，不能是纯色。
        const int cx0 = m_width / 2 - 2, cx1 = m_width / 2 + 2;
        const int cy0 = m_height / 2 - 2, cy1 = m_height / 2 + 2;

        for (int y = 0; y < m_height; ++y) {
            uint8_t* rowY = m_frame->data[0] + y * m_frame->linesize[0];
            const bool inHorizBar  = (y >= barY && y < barY + barH);
            const bool inCenterRow = (y >= cy0 && y <= cy1);
            for (int x = 0; x < m_width; ++x) {
                const bool inVertBar   = (x >= barX && x < barX + barW);
                const bool inCenterCol = (x >= cx0 && x <= cx1);
                rowY[x] = (inVertBar || inHorizBar || inCenterRow || inCenterCol) ? 200 : 40;
            }
        }
        // U/V 填中性值：YUV 里 (128,128) 表示无彩色
        for (int p = 1; p <= 2; ++p) {
            const int pw = m_width / 2, ph = m_height / 2;
            for (int y = 0; y < ph; ++y)
                memset(m_frame->data[p] + y * m_frame->linesize[p], 128, static_cast<size_t>(pw));
        }

        // ---- 时间戳：和真实设备那条路保持一致（微秒、单调、从 0 开始）----
        m_frame->pts      = av_rescale_q(m_frameIndex, AVRational{ 1, m_fps },
                                         AVRational{ 1, 1000000 });
        m_frame->duration = 1000000 / (m_fps > 0 ? m_fps : 30);
        ++m_frameIndex;

        // 交出去的是引用：调用方 av_frame_free 时这块缓冲不会被释放
        // （av_frame_ref 只加引用计数，跟播放器里 av_frame_clone 的用法同理）
        AVFrame* out = av_frame_alloc();
        if (!out) return nullptr;
        if (av_frame_ref(out, m_frame) < 0) { av_frame_free(&out); return nullptr; }
        return out;
    }

    int         width()  const override { return m_width; }
    int         height() const override { return m_height; }
    int         fps()    const override { return m_fps; }
    std::string pixelFormatName() const override { return "yuv420p(合成)"; }
    bool        sourceWasCompressed() const override { return false; }
    std::string describe() const override
    {
        return std::to_string(m_width) + "x" + std::to_string(m_height) + "@"
             + std::to_string(m_fps) + " 合成源(灰阶滚动十字)";
    }

private:
    VideoSourceConfig m_cfg;
    AVFrame*          m_frame = nullptr;
    int               m_width = 0, m_height = 0, m_fps = 0;
    int64_t           m_frameIndex = 0;
    std::atomic<bool> m_stop{ false };
    std::chrono::steady_clock::time_point m_startTime;
};

// ============================================================================================
// 音频源：真实设备（Windows: dshow，Linux: alsa）
// ============================================================================================

/*
 * 音频这条路的形状和视频完全一样，只有两点不同：
 *   1) 设备给的是 PCM。PCM 也是有"解码器"的（pcm_s16le 之类），
 *      走解码器才能拿到规规矩矩的 AVFrame（带 nb_samples / ch_layout / sample_rate），
 *      所以这里照样 avcodec_send_packet → avcodec_receive_frame；
 *   2) 时间戳按累计采样数算：pts = 已产出的样本数 / 采样率，
 *      这样天然无缝、单调，不受设备时钟漂移影响。
 *
 * 注意：这里不做重采样（不转采样格式/声道数），因为"目标格式"应该由编码器决定，
 * 那是 [第4步] 的 AAC 编码器用 swresample 干的事。
 */
class DeviceAudioSource : public IAudioSource
{
public:
    ~DeviceAudioSource() override { close(); }

    bool open(const AudioSourceConfig& cfg) override
    {
        close();
        m_cfg = cfg;

        const AVInputFormat* ifmt = av_find_input_format(cfg.inputFormat.c_str());
        if (!ifmt) {
            logWarn() << "[capture] 找不到音频输入格式" << cfg.inputFormat.c_str();
            return false;
        }

        // 和视频一样：先挂中断回调，再打开（否则打开这一步卡住就没法退出）
        m_fmt = avformat_alloc_context();
        if (!m_fmt) return false;
        m_fmt->interrupt_callback.callback = &interruptCb;
        m_fmt->interrupt_callback.opaque   = &m_stop;

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "sample_rate", std::to_string(cfg.sampleRate).c_str(), 0);   // 提议
        if (cfg.channels > 0) {
            // dshow 用 channels；alsa 用 ch_layout（新版 FFmpeg 的写法）。
            // 两个都设一下不会有害：设备/后端只认自己认识的那个。
            av_dict_set(&opts, "channels",   std::to_string(cfg.channels).c_str(), 0);
            av_dict_set(&opts, "ch_layout",  cfg.channels == 1 ? "mono" : "stereo", 0);
        }
        // [重点] audio_buffer_size（毫秒）是 dshow 专有旋钮：设备攒够这么多毫秒才给一块数据，
        // 默认 500ms —— 直播场景等于白送半秒延迟，这里压到 100ms。
        // 它是 dshow 专有选项，alsa/v4l2 不认，传了会被忽略（下面"设备忽略了参数"的日志会打出来），无害。
        // 别压到 50 以下：太小容易丢块，听感上是"噼啪"声。
        av_dict_set(&opts, "audio_buffer_size", "100", 0);

        int ret = avformat_open_input(&m_fmt, cfg.device.c_str(), ifmt, &opts);
        if (ret < 0) {
            logWarn() << "[capture] 打不开音频设备" << cfg.device.c_str() << ":" << errStr(ret).c_str();
            if (opts) av_dict_free(&opts);
            m_fmt = nullptr;
            return false;
        }
        if (opts) {
            AVDictionaryEntry* e = nullptr;
            while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) != nullptr)
                logWarn() << "[capture] 音频设备忽略了参数:" << e->key << "=" << e->value;
            av_dict_free(&opts);
        }

        ret = avformat_find_stream_info(m_fmt, nullptr);
        if (ret < 0) { logWarn() << "[capture] 音频 find_stream_info 失败:" << errStr(ret).c_str(); return false; }

        const AVCodec* decoder = nullptr;
        m_audioStreamIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (m_audioStreamIndex < 0 || !decoder) {
            logWarn() << "[capture] 这个设备没有音频流:" << cfg.device.c_str();
            return false;
        }
        AVStream* st = m_fmt->streams[m_audioStreamIndex];
        AVCodecParameters* par = st->codecpar;

        m_sampleRate = par->sample_rate;
        m_channels   = par->ch_layout.nb_channels;   // 新版 FFmpeg 用 ch_layout（旧的 channels 字段已废弃）
        m_sampleFmt  = static_cast<AVSampleFormat>(par->format);

        m_decCtx = avcodec_alloc_context3(decoder);
        if (!m_decCtx) return false;
        avcodec_parameters_to_context(m_decCtx, par);
        m_decCtx->pkt_timebase = st->time_base;
        ret = avcodec_open2(m_decCtx, decoder, nullptr);
        if (ret < 0) { logWarn() << "[capture] 音频 avcodec_open2 失败:" << errStr(ret).c_str(); return false; }

        m_decodedFrame = av_frame_alloc();
        m_packet       = av_packet_alloc();
        if (!m_decodedFrame || !m_packet) return false;

        logInfo().nospace() << "[capture] 音频设备打开成功: " << cfg.device.c_str()
                          << "  实际 " << m_sampleRate << "Hz " << m_channels << "声道 "
                          << sampleName(m_sampleFmt).c_str();
        return true;
    }

    void close() override
    {
        if (m_packet)       av_packet_free(&m_packet);
        if (m_decodedFrame) av_frame_free(&m_decodedFrame);
        if (m_decCtx)       avcodec_free_context(&m_decCtx);
        if (m_fmt)          avformat_close_input(&m_fmt);
        m_audioStreamIndex = -1;
    }

    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {
        if (!m_fmt) return nullptr;

        while (true) {
            if (m_stop.load()) return nullptr;

            int ret = av_read_frame(m_fmt, m_packet);
            if (ret < 0) {
                if (ret != AVERROR_EXIT)
                    logWarn() << "[capture] 音频 av_read_frame 结束:" << errStr(ret).c_str();
                return nullptr;
            }
            if (m_packet->stream_index != m_audioStreamIndex) { av_packet_unref(m_packet); continue; }

            ret = avcodec_send_packet(m_decCtx, m_packet);
            av_packet_unref(m_packet);
            if (ret < 0) { logWarn() << "[capture] 音频 send_packet 失败:" << errStr(ret).c_str(); continue; }

            while (true) {
                ret = avcodec_receive_frame(m_decCtx, m_decodedFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) { logWarn() << "[capture] 音频 receive_frame 失败:" << errStr(ret).c_str(); break; }

                AVFrame* out = av_frame_alloc();
                if (!out) return nullptr;
                if (av_frame_ref(out, m_decodedFrame) < 0) { av_frame_free(&out); break; }
                av_frame_unref(m_decodedFrame);

                // 时间戳 = 已产出样本数 / 采样率（单位微秒）。这样音频时间轴天然连续无缝。
                out->pts = av_rescale_q(m_totalSamples, AVRational{ 1, m_sampleRate },
                                        AVRational{ 1, 1000000 });
                m_totalSamples += out->nb_samples;
                return out;
            }
        }
    }

    int         sampleRate() const override { return m_sampleRate; }
    int         channels()   const override { return m_channels; }
    AVSampleFormat sampleFormat() const override { return m_sampleFmt; }
    std::string sampleFormatName() const override { return sampleName(m_sampleFmt); }
    std::string describe() const override
    {
        return std::to_string(m_sampleRate) + "Hz " + std::to_string(m_channels) + "声道 "
             + sampleName(m_sampleFmt);
    }

private:
    AudioSourceConfig m_cfg;
    AVFormatContext*  m_fmt = nullptr;
    AVCodecContext*   m_decCtx = nullptr;
    AVFrame*          m_decodedFrame = nullptr;
    AVPacket*         m_packet = nullptr;
    int               m_audioStreamIndex = -1;
    int               m_sampleRate = 0, m_channels = 0;
    AVSampleFormat    m_sampleFmt = AV_SAMPLE_FMT_NONE;
    std::atomic<bool> m_stop{ false };
    int64_t           m_totalSamples = 0;
};

// ============================================================================================
// 音频源二：内置正弦波（不需要麦克风，"sine"）
// ============================================================================================

/*
 * 为什么要有它：
 *   - 你可以没有麦克风（或者在无桌面/CI 环境里）验证"AAC 编码 → 音视频进同一个容器"这条链；
 *   - "容器里到底有没有音频流、时间戳对不对齐"用它一眼就能验；
 *   - 做参数实验时它比麦克风更可控（固定 440Hz / 固定 48kHz）。
 *
 * 输出固定为 s16（16 位整数、交错），这也正是多数设备的原生格式；
 * "转成 AAC 要的 fltp"交给编码器里的 swresample —— 和真实麦克风走的是完全同一条路。
 */
class SyntheticAudioSource : public IAudioSource
{
public:
    ~SyntheticAudioSource() override = default;

    bool open(const AudioSourceConfig& cfg) override
    {
        m_sampleRate = cfg.sampleRate > 0 ? cfg.sampleRate : 48000;
        m_channels   = cfg.channels   > 0 ? cfg.channels   : 2;
        // 每次给 1024 个采样（48kHz 下约 21ms），和真实设备的分片大小一个量级
        m_samplesPerFrame = 1024;
        m_nextTime = std::chrono::steady_clock::now();
        logInfo().nospace() << "[capture] 合成音频源(sine) 启动: " << m_sampleRate << "Hz "
                          << m_channels << "声道 440Hz 正弦 s16（不需要麦克风）";
        return true;
    }

    void close() override {}
    void requestStop() override { m_stop.store(true); }

    AVFrame* readFrame() override
    {

        // ---- 按采样率节流 ----
        // 生成音频比采集快得多，不节流就会"一秒钟灌进去几十秒的音频"，把编码器打爆。
        // 用绝对时刻推进，并且落后了就对齐到现在（避免积压后疯狂追帧）。
        const auto period = std::chrono::microseconds(
            static_cast<long long>(m_samplesPerFrame) * 1000000 / m_sampleRate);
        m_nextTime += period;
        const auto now = std::chrono::steady_clock::now();
        if (m_nextTime < now) m_nextTime = now;
        std::this_thread::sleep_until(m_nextTime);
        if (m_stop.load()) return nullptr;

        AVFrame* f = av_frame_alloc();
        if (!f) return nullptr;
        f->format      = AV_SAMPLE_FMT_S16;      // 交错 16 位
        f->sample_rate = m_sampleRate;
        f->nb_samples  = m_samplesPerFrame;
        // 声道布局要用 API 设（C++ 里不能用 AV_CHANNEL_LAYOUT_STEREO 那种 C 的复合字面量宏）
        av_channel_layout_default(&f->ch_layout, m_channels);
        if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return nullptr; }

        // ---- 生成正弦波 ----
        // 44.1kHz/48kHz 下 440Hz 的周期不是整数个采样，所以用一个 double 相位累加器。
        const double kPi = 3.14159265358979323846;   // 自己定义，免得依赖 M_PI（MSVC 上还要额外宏）
        const double step = 2.0 * kPi * 440.0 / m_sampleRate;
        int16_t* p = reinterpret_cast<int16_t*>(f->data[0]);
        for (int i = 0; i < m_samplesPerFrame; ++i) {
            // 幅度取满量程的 1/4，避免削顶（也方便用耳朵分辨有没有爆音）
            const int16_t v = static_cast<int16_t>(std::sin(m_phase) * 8000.0);
            m_phase += step;
            if (m_phase > 2.0 * kPi) m_phase -= 2.0 * kPi;
            for (int c = 0; c < m_channels; ++c)
                *p++ = v;
        }
        m_sampleIndex += m_samplesPerFrame;

        // 时间戳和真实音频源保持一致：按累计样本数算，单位微秒
        f->pts = av_rescale_q(m_totalSamples, AVRational{ 1, m_sampleRate },
                              AVRational{ 1, 1000000 });
        m_totalSamples += m_samplesPerFrame;
        return f;
    }

    int            sampleRate() const override { return m_sampleRate; }
    int            channels()   const override { return m_channels; }
    AVSampleFormat sampleFormat() const override { return AV_SAMPLE_FMT_S16; }
    std::string    sampleFormatName() const override { return "s16(合成)"; }
    std::string    describe() const override
    {
        return std::to_string(m_sampleRate) + "Hz " + std::to_string(m_channels)
             + "声道 440Hz 正弦(合成)";
    }

private:
    int   m_sampleRate = 48000;
    int   m_channels = 2;
    int   m_samplesPerFrame = 1024;
    int64_t m_sampleIndex = 0;
    int64_t m_totalSamples = 0;
    double  m_phase = 0.0;
    std::atomic<bool> m_stop{ false };
    std::chrono::steady_clock::time_point m_nextTime;
};

// ============================================================================================
// 工厂
// ============================================================================================

/*
 * 设备注册 —— 这是个很容易踩的坑，值得单独说清楚：
 *
 *   - FFmpeg 里"文件/网络"的 demuxer 是自动注册的，所以你的播放器从来不需要注册任何东西；
 *   - 但"设备"（dshow / v4l2 / alsa / avfoundation…）要显式注册一次，
 *     否则 av_find_input_format("dshow") 会返回 nullptr，报"找不到输入格式"。
 *   - 注意别和 已被删除 的 av_register_all() 搞混：那个删了，这个还在，而且必须调。
 *
 * 用函数内静态变量保证只注册一次：C++11 起局部静态的初始化是线程安全的，
 * 所以多个采集线程同时进来也只会执行一次。
 */
static void ensureDeviceRegistered()
{
    static const bool registered = [] { avdevice_register_all(); return true; }();
    (void)registered;
}

std::unique_ptr<IVideoSource> createVideoSource(const VideoSourceConfig& cfg)
{
    ensureDeviceRegistered();
    // 合成源：不需要硬件，CI 里也能跑
    if (cfg.inputFormat == "testsrc")
        return std::make_unique<SyntheticVideoSource>();
    // 真实设备：Windows=dshow、Linux=v4l2（同一个实现，靠 inputFormat 区分）
    return std::make_unique<DeviceVideoSource>();
}

std::unique_ptr<IAudioSource> createAudioSource(const AudioSourceConfig& cfg)
{
    ensureDeviceRegistered();
    // 合成音频：不需要麦克风，音视频全链路都能验
    if (cfg.inputFormat == "sine")
        return std::make_unique<SyntheticAudioSource>();
    // 真实设备：Windows=dshow、Linux=alsa
    return std::make_unique<DeviceAudioSource>();
}

// ============================================================================================
// 工具实现
// ============================================================================================

int listInputDevices(const std::string& inputFormat)
{
    ensureDeviceRegistered();
    const AVInputFormat* ifmt = av_find_input_format(inputFormat.c_str());
    if (!ifmt) {
        logWarn() << "[devices] 找不到输入格式" << inputFormat.c_str()
                   << "；这份 FFmpeg 可能没编进该设备";
        return -1;
    }

    // avdevice_list_input_sources 会真的去问系统（Windows 走 DirectShow 枚举、
    // Linux 走 v4l2/alsa 枚举），所以能列出设备名——这个名字就是后面 --video/--audio 要写的东西。
    AVDeviceInfoList* list = nullptr;
    const int ret = avdevice_list_input_sources(ifmt, nullptr, nullptr, &list);
    if (ret < 0 || !list) {
        logWarn() << "[devices] 枚举失败:" << errStr(ret).c_str();
        return ret;
    }

    logInfo().nospace() << "[devices] " << inputFormat.c_str()
                      << " 共 " << list->nb_devices << " 个设备:";
    for (int i = 0; i < list->nb_devices; ++i) {
        const AVDeviceInfo* d = list->devices[i];
        // 注意 dshow 的两个字段：
        //   device_name        = 备用名（@device_sw_{GUID} 这种，难看但绝对唯一）
        //   device_description = 友好名（"AMD Privacy View camera"），可以直接写进命令行
        // 所以这里把友好名当主名字打印，备用名放括号里备查。
        const std::string alt   = d && d->device_name ? d->device_name : "?";
        const std::string human = d && d->device_description && *d->device_description
                                      ? d->device_description : alt;
        // 按类型给一个"可以直接粘到命令行"的写法：dshow 要求 input 名字形如 video=名字 / audio=名字
        // 扫全部 media_types，而不是只看 [0]：有的设备 [0] 不是我们关心的类型，
        // 有的设备 media_types 干脆为空（曾因此漏打 audio= 前缀，把使用者带偏）。
        const char* prefix = "";
        bool typeKnown = false;
        if (inputFormat == "dshow" && d && d->media_types) {
            for (int t = 0; t < d->nb_media_types; ++t) {
                if (d->media_types[t] == AVMEDIA_TYPE_VIDEO) { prefix = "video="; typeKnown = true; break; }
                if (d->media_types[t] == AVMEDIA_TYPE_AUDIO) { prefix = "audio="; typeKnown = true; break; }
            }
        }
        const char* hint = (inputFormat == "dshow" && !typeKnown)
                               ? "   (类型未知，可试 audio=名字 或 video=名字)" : "";
        logInfo().nospace() << "  [" << i << "] " << prefix << human.c_str() << hint
                          << (human == alt ? "" : ("   (备用名: " + alt + ")").c_str());
    }
    avdevice_free_list_devices(&list);
    return 0;
}

bool saveFrameAsPpm(const AVFrame* yuv420pFrame, const std::string& path)
{
    if (!yuv420pFrame || yuv420pFrame->format != AV_PIX_FMT_YUV420P) {
        logWarn() << "[ppm] 只支持 yuv420p 输入";
        return false;
    }
    const int w = yuv420pFrame->width, h = yuv420pFrame->height;

    // YUV → RGB24：PPM(P6) 是 RGB 格式，所以这里再借一次 swscale（只做颜色空间转换）
    SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_YUV420P,
                                     w, h, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;

    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    uint8_t* dstData[4]      = { rgb.data(), nullptr, nullptr, nullptr };
    int      dstLinesize[4]  = { w * 3, 0, 0, 0 };
    sws_scale(sws, yuv420pFrame->data, yuv420pFrame->linesize, 0, h, dstData, dstLinesize);
    sws_freeContext(sws);

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { logWarn() << "[ppm] 无法写文件:" << path.c_str(); return false; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);          // PPM 头：P6=二进制 RGB，紧接宽高和最大值
    fwrite(rgb.data(), 1, rgb.size(), fp);
    fclose(fp);
    logInfo() << "[ppm] 已保存采集帧:" << path.c_str() << w << "x" << h;
    return true;
}
