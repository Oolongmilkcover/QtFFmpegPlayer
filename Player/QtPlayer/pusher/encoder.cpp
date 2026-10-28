#include "encoder.h"

#include <QDebug>

extern "C" {
#include <libavutil/audio_fifo.h>      // 音频凑帧：AAC 每帧必须正好 frame_size(1024) 个采样
#include <libavutil/channel_layout.h>  // AVChannelLayout 相关辅助
#include <libavutil/opt.h>             // av_opt_set：设置编码器的私有选项（preset / tune）
#include <libavutil/pixdesc.h>         // av_get_pix_fmt_name
#include <libswresample/swresample.h>  // 音频重采样/重格式（设备给 s16，AAC 要 fltp）
}

#include <string>

static std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

/*
 * 把编码器当前能产出的包全部取出来，交给 sink。
 *
 * 视频和音频两条路完全一样（送完输入 → 循环收包），所以提出来共用：
 * 调用方只要告诉我"哪个编码器、包算第几路流、交给谁"。
 *
 * 两个返回码必须区分（新手最常在这里写错）：
 *   AVERROR(EAGAIN) = 还要更多输入才会出包，不是错误；
 *   AVERROR_EOF     = 已经 flush 干净了，也不是错误。
 */
static bool drainToSink(AVCodecContext* cc, const PacketSink& sink)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    bool ok = true;
    while (true) {
        const int ret = avcodec_receive_packet(cc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            qWarning() << "[encoder] avcodec_receive_packet 失败:" << errStr(ret).c_str();
            ok = false;
            break;
        }
        // stream_index 由编排层（Pusher 的 sink）按实际流号赋值，编码器不猜
        if (sink) sink(pkt);      // 借用指针：sink 要留就得自己 av_packet_clone
        av_packet_unref(pkt);     // 交回引用，pkt 结构体留着复用
    }

    av_packet_free(&pkt);
    return ok;
}

/*
 * ============================ x264 视频编码器 ============================
 *
 * 它的职责就一句话：把采集来的 yuv420p 帧，编成 H.264 的 AVPacket。
 *
 * [第2步] 的四件关键事（都在下面的代码里标了 [重点]）：
 *   1) 参数：宽高 / pix_fmt / time_base / framerate / gop_size / max_b_frames / bit_rate
 *   2) 时间基换算：输入帧的 pts 是微秒，编码器要 1/fps —— 用 av_rescale_q 换算，
 *      不换算就会得到"时长离谱、播放器 seek 乱跳"的文件
 *   3) 零拷贝喂帧：av_frame_ref 做浅引用，只改 pts，不去动调用方的帧
 *   4) flush：send_frame(nullptr) 把编码器内部压着的包全部吐出来，否则会丢尾部若干帧
 *
 * 两种"时间戳"再明确一次：
 *   输入（我们自己的约定）：微秒，{1,1000000}
 *   编码器内部：{1, fps}（一帧一格）
 *   输出流（muxer 那边负责）：mp4/flv 各自的规矩
 */
namespace {

class X264VideoEncoder : public IVideoEncoder
{
public:
    ~X264VideoEncoder() override { close(); }

    bool open(const EncoderConfig& cfg) override
    {
        close();
        m_cfg = cfg;

        // ① 找编码器。
        //    用 by_name("libx264") 而不是 avcodec_find_encoder(AV_CODEC_ID_H264)：
        //    后者可能拿到别的 H.264 实现（比如某些平台的硬件封装），我们要的是明确的 libx264。
        const AVCodec* codec = avcodec_find_encoder_by_name(cfg.videoCodec.c_str());
        if (!codec) {
            qWarning() << "[encoder] 找不到编码器" << cfg.videoCodec.c_str()
                       << "（这份 FFmpeg 可能没编进 libx264，可用 ffmpeg -encoders 确认）";
            return false;
        }

        m_cc = avcodec_alloc_context3(codec);
        if (!m_cc) return false;

        // ② 基本参数
        m_cc->width        = cfg.width;
        m_cc->height       = cfg.height;
        // [重点] 必须和喂进来的帧格式一致。采集源已经统一转成 yuv420p，所以这里固定它。
        m_cc->pix_fmt      = AV_PIX_FMT_YUV420P;
        // [重点] 编码器的时间基 = 1/fps，也就是"pts 用帧号表示"。这是编码器最常见的约定。
        m_cc->time_base    = AVRational{ 1, cfg.fps > 0 ? cfg.fps : 30 };
        // 帧率会写进码流（VUI），播放器靠它推算显示时间，别漏
        m_cc->framerate    = AVRational{ cfg.fps > 0 ? cfg.fps : 30, 1 };
        // 关键帧间隔：越小 seek 越精确、起播越快，代价是码率略升（[第5步] 要拿它做实验）
        m_cc->gop_size     = cfg.gopSize;
        // B 帧：会让 dts<pts（封装时要按 dts 排序）且引入额外缓冲，推流先给 0
        m_cc->max_b_frames = cfg.maxBFrames;
        m_cc->bit_rate     = cfg.videoBitrate;

        // ③ SPS/PPS 要作为 extradata 交给 muxer（mp4/flv 都要求这样），
        //    不设这个标志，出来的文件在某些播放器里会花屏或直接打不开。
        m_cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // ④ x264 的私有选项（priv_data 就是 x264 的参数表）
        //    preset 决定"压缩率 ↔ 速度"的档位；tune=zerolatency 关掉前瞻/缓冲，换低延迟
        if (!cfg.preset.empty())
            av_opt_set(m_cc->priv_data, "preset", cfg.preset.c_str(), 0);
        if (cfg.zerolatency)
            av_opt_set(m_cc->priv_data, "tune", "zerolatency", 0);

        if (cfg.cbr) {
            // "接近 CBR"：把码率上下限钉在目标值上，并让 x264 打开 HRD 占用率模型。
            // 做码率稳定性实验（[第5步]）够用；如果要严格 CBR（码率完全平直），
            // 还得让 x264 填充空包：av_opt_set(priv, "x264-params", "nal-hrd=cbr", 0) 已包含在下面这行里。
            m_cc->rc_min_rate    = cfg.videoBitrate;
            m_cc->rc_max_rate    = cfg.videoBitrate;
            m_cc->rc_buffer_size = cfg.videoBitrate;        // 约 1 秒的缓冲
            av_opt_set(m_cc->priv_data, "x264-params", "nal-hrd=cbr:force-cfr=1", 0);
        }

        // ⑤ 打开编码器
        int ret = avcodec_open2(m_cc, codec, nullptr);
        if (ret < 0) {
            qWarning() << "[encoder] avcodec_open2 失败:" << errStr(ret).c_str();
            return false;
        }

        // ⑥ 导出一份 AVCodecParameters 给 muxer 建流用
        //    （muxer 只需要"描述"，不需要这个编码器实例）
        m_par = avcodec_parameters_alloc();
        if (!m_par) return false;
        ret = avcodec_parameters_from_context(m_par, m_cc);
        if (ret < 0) {
            qWarning() << "[encoder] avcodec_parameters_from_context 失败:" << errStr(ret).c_str();
            return false;
        }

        qInfo().nospace() << "[encoder] 打开成功: " << cfg.videoCodec.c_str()
                          << " " << cfg.width << "x" << cfg.height << "@" << cfg.fps
                          << " bitrate=" << cfg.videoBitrate
                          << " gop=" << cfg.gopSize
                          << " bframes=" << cfg.maxBFrames
                          << " preset=" << cfg.preset.c_str()
                          << (cfg.zerolatency ? " tune=zerolatency" : "")
                          << " 编码器时间基=" << m_cc->time_base.num << "/" << m_cc->time_base.den;
        return true;
    }

    void close()
    {
        if (m_par) avcodec_parameters_free(&m_par);
        if (m_cc)  avcodec_free_context(&m_cc);
    }

    void setSink(PacketSink sink) override { m_sink = std::move(sink); }

    bool encode(AVFrame* inYuv420p) override
    {
        if (!m_cc || !inYuv420p) return false;

        // 防御：格式/尺寸不一致的话编码器会报错甚至崩，这里直接丢掉这一帧并给出提示
        if (inYuv420p->format != m_cc->pix_fmt
            || inYuv420p->width != m_cc->width || inYuv420p->height != m_cc->height) {
            qWarning() << "[encoder] 输入帧和编码器不匹配（格式或尺寸），丢弃这一帧";
            return false;
        }

        // [重点] 零拷贝地换时间基：
        //   av_frame_ref 只把"像素缓冲的引用"复制一份（不拷像素），
        //   于是在新帧上改 pts 不会污染调用方的帧。
        AVFrame* feed = av_frame_alloc();
        if (!feed) return false;
        if (av_frame_ref(feed, inYuv420p) < 0) { av_frame_free(&feed); return false; }

        // [重点] 时间基换算：{1,1000000}(微秒) → {1,fps}(帧号)
        feed->pts = av_rescale_q(inYuv420p->pts, m_cfg.inputTimeBase, m_cc->time_base);
        feed->duration = inYuv420p->duration > 0
                             ? av_rescale_q(inYuv420p->duration, m_cfg.inputTimeBase, m_cc->time_base)
                             : 1;   // 通常是"一帧一格"

        const int ret = avcodec_send_frame(m_cc, feed);
        // send 之后就能还引用了：编码器内部已经把自己需要的数据引用住（依然是零拷贝）
        av_frame_free(&feed);
        if (ret < 0) {
            qWarning() << "[encoder] avcodec_send_frame 失败:" << errStr(ret).c_str();
            return false;
        }

        // 送进去一帧，可能立刻能取出一包（也可能要等几帧才出包）
        if (!drainPackets()) return false;
        ++m_framesIn;
        return true;
    }

    void flush() override
    {
        if (!m_cc) return;
        // send(nullptr) = 告诉编码器"没有新帧了"，它会把内部缓冲的包全部产出来
        avcodec_send_frame(m_cc, nullptr);
        drainPackets();
    }

    AVCodecParameters* codecpar() override { return m_par; }

    AVRational timeBase() const override
    {
        return m_cc ? m_cc->time_base : AVRational{ 1, 30 };
    }

private:
    // 视频流固定是第 0 路（音频那路是 1）；取包的循环和音频共用 drainToSink()
    bool drainPackets() { return drainToSink(m_cc, m_sink); }

    EncoderConfig      m_cfg;
    PacketSink         m_sink;
    AVCodecContext*    m_cc = nullptr;
    AVCodecParameters* m_par = nullptr;
    int64_t            m_framesIn = 0;
};

// ============================ AAC 音频编码器（[第4步]）============================

/*
 * 比视频那一路多两件"做不对声音就是坏的"的事：
 *
 *   1) 格式转换（swresample）：麦克风一般给 s16（16 位整数、交错），
 *      而 AAC 编码器只吃 fltp（32 位浮点、平面）；采样率/声道数也可能不同。用 SwrContext 一次转好。
 *
 *   2) 凑齐一帧（AVAudioFifo）：AAC 每帧必须正好 frame_size（通常 1024）个采样，
 *      而设备每次给的样本数不固定（1024 / 480 / 882 都可能）。
 *      所以先用 FIFO 攒着，够 1024 才取一帧去编 —— 少了这一步，编码器要么报错要么一直没输出。
 *
 * 时间戳：编码器的时间基是 {1, sample_rate}，所以 pts 直接用"已产出的样本数"，
 *        天然正确又单调（和采集端"按样本数算时间"是同一条思路）。
 */
class AacAudioEncoder : public IAudioEncoder
{
public:
    ~AacAudioEncoder() override { close(); }

    bool open(const EncoderConfig& cfg) override
    {
        close();
        m_cfg = cfg;

        const AVCodec* codec = avcodec_find_encoder_by_name(cfg.audioCodec.c_str());
        if (!codec) {
            qWarning() << "[encoder] 找不到音频编码器" << cfg.audioCodec.c_str()
                       << "（用 ffmpeg -encoders 看这份 FFmpeg 有没有 aac）";
            return false;
        }

        m_inSampleRate = cfg.audioInSampleRate > 0 ? cfg.audioInSampleRate : 48000;
        const int inChannels = cfg.audioInChannels > 0 ? cfg.audioInChannels : 2;

        m_cc = avcodec_alloc_context3(codec);
        if (!m_cc) return false;

        m_cc->sample_rate = m_inSampleRate;
        // 声道布局默认跟输入一致（AAC 支持 mono / stereo / 5.1 等）
        av_channel_layout_default(&m_cc->ch_layout, inChannels);
        // 采样格式用编码器支持的第一种（AAC 实际上只有 fltp）
        m_cc->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        m_cc->bit_rate   = cfg.audioBitrate;
        m_cc->time_base  = AVRational{ 1, m_inSampleRate };     // 音频时间基 = 1/采样率
        m_cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;             // AudioSpecificConfig 作为 extradata 交给 muxer

        int ret = avcodec_open2(m_cc, codec, nullptr);
        if (ret < 0) {
            qWarning() << "[encoder] 音频 avcodec_open2 失败:" << errStr(ret).c_str();
            return false;
        }
        if (m_cc->frame_size <= 0) m_cc->frame_size = 1024;     // AAC-LC 固定 1024

        // ---- swresample：设备格式 → 编码器格式 ----
        AVChannelLayout inLayout;
        av_channel_layout_default(&inLayout, inChannels);
        ret = swr_alloc_set_opts2(&m_swr,
                                  &m_cc->ch_layout, m_cc->sample_fmt, m_cc->sample_rate,  // 输出：编码器要的
                                  &inLayout, cfg.audioInSampleFmt, m_inSampleRate,         // 输入：设备给的
                                  0, nullptr);
        av_channel_layout_uninit(&inLayout);
        if (ret < 0 || !m_swr) {
            qWarning() << "[encoder] swr_alloc_set_opts2 失败:" << errStr(ret).c_str();
            return false;
        }
        ret = swr_init(m_swr);
        if (ret < 0) {
            qWarning() << "[encoder] swr_init 失败:" << errStr(ret).c_str();
            return false;
        }

        // ---- FIFO：攒够一帧再编 ----
        m_fifo = av_audio_fifo_alloc(m_cc->sample_fmt, m_cc->ch_layout.nb_channels, m_cc->frame_size);
        if (!m_fifo) return false;

        m_par = avcodec_parameters_alloc();
        if (!m_par || avcodec_parameters_from_context(m_par, m_cc) < 0) {
            qWarning() << "[encoder] 音频参数导出失败";
            return false;
        }

        qInfo().nospace() << "[encoder] 音频打开成功: " << cfg.audioCodec.c_str()
                          << " " << m_cc->sample_rate << "Hz " << m_cc->ch_layout.nb_channels << "声道 "
                          << av_get_sample_fmt_name(m_cc->sample_fmt)
                          << " 码率=" << cfg.audioBitrate
                          << " 每帧采样=" << m_cc->frame_size
                          << "  输入=" << m_inSampleRate << "Hz "
                          << av_get_sample_fmt_name(cfg.audioInSampleFmt);
        return true;
    }

    void close()
    {
        if (m_fifo) av_audio_fifo_free(m_fifo);
        if (m_swr)  swr_free(&m_swr);
        if (m_par)  avcodec_parameters_free(&m_par);
        if (m_cc)   avcodec_free_context(&m_cc);
    }

    void setSink(PacketSink sink) override { m_sink = std::move(sink); }

    bool encode(AVFrame* in) override
    {
        if (!m_cc || !m_swr || !m_fifo || !in) return false;

        // 1) 估算重采样之后会得到多少个采样。
        //    要把 swr 内部残留的延迟算进去，否则反复调用会慢慢漏样本（声音会越来越"快"）。
        const int64_t delay = swr_get_delay(m_swr, m_inSampleRate);
        const int cap = static_cast<int>(
            av_rescale_rnd(delay + in->nb_samples, m_cc->sample_rate, m_inSampleRate, AV_ROUND_UP));

        // 2) 转换到一个临时帧（swr 对"交错 → 平面"的差异都在这一步解决）
        AVFrame* converted = allocFrame(cap > 0 ? cap : 1);
        if (!converted) return false;
        const int got = swr_convert(m_swr, converted->data, cap > 0 ? cap : 1,
                                    const_cast<const uint8_t**>(in->extended_data), in->nb_samples);
        if (got < 0) {
            qWarning() << "[encoder] swr_convert 失败:" << errStr(got).c_str();
            av_frame_free(&converted);
            return false;
        }
        converted->nb_samples = got;

        // 3) 全部塞进 FIFO（容量不够时 av_audio_fifo_write 会自己扩容）
        if (got > 0) {
            const int written =
                av_audio_fifo_write(m_fifo, reinterpret_cast<void**>(converted->extended_data), got);
            if (written < got) {
                qWarning() << "[encoder] FIFO 写入不足，丢弃这一帧";
                av_frame_free(&converted);
                return false;
            }
        }
        av_frame_free(&converted);

        // 4) 每凑满一帧就编一帧出去
        return encodeFullFrames();
    }

    void flush() override
    {
        if (!m_cc) return;

        // FIFO 里剩下不足一帧的样本：补静音凑满再编，否则结尾会丢掉一小段声音
        const int rest = m_fifo ? av_audio_fifo_size(m_fifo) : 0;
        if (rest > 0) {
            AVFrame* pad = allocFrame(m_cc->frame_size);   // 缓冲先被填成静音
            if (pad) {
                av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(pad->data), rest);
                pad->pts = m_nextPts;
                m_nextPts += pad->nb_samples;
                avcodec_send_frame(m_cc, pad);
                drainToSink(m_cc, m_sink);
                av_frame_free(&pad);
            }
        }

        avcodec_send_frame(m_cc, nullptr);      // 告诉编码器：没有新样本了
        drainToSink(m_cc, m_sink);
    }

    AVCodecParameters* codecpar() override { return m_par; }
    AVRational timeBase() const override
    {
        return m_cc ? m_cc->time_base : AVRational{ 1, 48000 };
    }

private:
    // 分配一个"编码器格式 + nbSamples 个采样"的帧，并把缓冲清零（等于静音）
    AVFrame* allocFrame(int nbSamples)
    {
        AVFrame* f = av_frame_alloc();
        if (!f) return nullptr;
        f->format      = m_cc->sample_fmt;
        f->sample_rate = m_cc->sample_rate;
        f->nb_samples  = nbSamples;
        // 用 API 拷贝声道布局（自定义布局时内部会有指针，直接结构体赋值不安全）
        if (av_channel_layout_copy(&f->ch_layout, &m_cc->ch_layout) < 0) {
            av_frame_free(&f);
            return nullptr;
        }
        if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return nullptr; }
        // av_frame_get_buffer 不保证清零，显式填静音（flush 时补尾巴要用到这一点）
        av_samples_set_silence(f->data, 0, nbSamples, m_cc->ch_layout.nb_channels, m_cc->sample_fmt);
        return f;
    }

    // FIFO 里够一帧就取一帧送进编码器
    bool encodeFullFrames()
    {
        bool ok = true;
        while (av_audio_fifo_size(m_fifo) >= m_cc->frame_size) {
            AVFrame* f = allocFrame(m_cc->frame_size);
            if (!f) { ok = false; break; }

            if (av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(f->data), m_cc->frame_size)
                < m_cc->frame_size) {
                av_frame_free(&f);
                ok = false;
                break;
            }

            f->pts = m_nextPts;          // pts = 样本计数（时间基就是 1/采样率）
            m_nextPts += f->nb_samples;

            const int ret = avcodec_send_frame(m_cc, f);
            av_frame_free(&f);
            if (ret < 0) {
                qWarning() << "[encoder] 音频 avcodec_send_frame 失败:" << errStr(ret).c_str();
                ok = false;
                break;
            }
            if (!drainToSink(m_cc, m_sink)) { ok = false; break; }
        }
        return ok;
    }

    EncoderConfig      m_cfg;
    PacketSink         m_sink;
    AVCodecContext*    m_cc = nullptr;
    AVCodecParameters* m_par = nullptr;
    SwrContext*        m_swr = nullptr;
    AVAudioFifo*       m_fifo = nullptr;
    int                m_inSampleRate = 48000;
    int64_t            m_nextPts = 0;
};

} // namespace

std::unique_ptr<IVideoEncoder> createVideoEncoder(const EncoderConfig& cfg)
{
    // 以后要加硬件编码（NVENC / QSV / VideoToolbox / MediaCodec），在这里按 cfg.videoCodec 分支即可，
    // 上层（Pusher）完全不用改 —— 这就是接口存在的意义。
    Q_UNUSED(cfg);
    return std::make_unique<X264VideoEncoder>();
}

std::unique_ptr<IAudioEncoder> createAudioEncoder(const EncoderConfig& cfg)
{
    Q_UNUSED(cfg);
    return std::make_unique<AacAudioEncoder>();
}
