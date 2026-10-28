#include "muxer.h"

#include "log.h"

extern "C" {
#include <libavutil/opt.h>
}

#include <atomic>
#include <string>
#include <vector>

static std::string errStr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// AVIO 的中断回调：让"卡在网络上"的写操作能被叫停（[第3步] 推流时会用到）
static int interruptCb(void* opaque)
{
    return static_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}

/*
 * ============================ 格式封装器 ============================
 *
 * 同一个实现同时支持两种输出，差别只在 OutputConfig：
 *   format="mp4" + url="out.mp4"              → 落盘（[第2步] 先验这条）
 *   format="flv" + url="rtmp://host/live/key" → 推流（[第3步] 只改这两个字段）
 *
 * 三件容易错的事：
 *   ① 时间基：编码器给包的 pts 是 {1,fps}，输出流要的是自己的规矩（FLV=毫秒，mp4 常被改写成 1/90000）。
 *      所以 addStream 里记住"进来的时间基"，writePacket 里现查 st->time_base 做换算。
 *      ——注意是 write_header 之后查，因为 muxer 会趁机改写 st->time_base。
 *   ② 落盘时用 av_interleaved_write_frame 让 muxer 按 dts 交织（音视频以后要一起写）。
 *   ③ trailer 必须写：mp4 的 moov 箱在 trailer 里，不写的话文件基本没法播。
 */
namespace {

class FormatMuxer : public IMuxer
{
public:
    ~FormatMuxer() override
    {
        // 兜底：如果上层忘了 writeTrailer（异常路径），这里补一次，避免 mp4 缺 moov
        writeTrailer();
        close();
    }

    bool open(const OutputConfig& cfg, std::atomic<bool>* abortFlag) override
    {
        close();
        m_cfg = cfg;
        // 借入编排层的叫停标志（为空则用自己的兜底标志），并复位它
        m_abortFlag = abortFlag;
        if (m_abortFlag) m_abortFlag->store(false);

        // ① 建输出上下文。format 为空时 FFmpeg 按 url 猜（.mp4 → mp4，rtmp:// → flv）
        int ret = avformat_alloc_output_context2(&m_out, nullptr,
                                                 cfg.format.empty() ? nullptr : cfg.format.c_str(),
                                                 cfg.url.c_str());
        if (ret < 0 || !m_out) {
            logWarn() << "[muxer] avformat_alloc_output_context2 失败:" << errStr(ret).c_str();
            return false;
        }

        // ② 中断回调（网络输出时必须有；落盘时挂着也无害）
        m_out->interrupt_callback.callback = &interruptCb;
        m_out->interrupt_callback.opaque   = m_abortFlag ? m_abortFlag : &m_abortSelf;

        // ③ 打开输出 IO。像 "null" 这类格式是 AVFMT_NOFILE，不需要 IO。
        if (!(m_out->oformat->flags & AVFMT_NOFILE)) {
            AVDictionary* opts = nullptr;
            // rw_timeout 单位是微秒：网络卡住超过这个时间就让 avio 报错返回，
            // 而不是无限期挂着（配合中断回调，关程序时也能立刻退出）
            av_dict_set(&opts, "rw_timeout",
                        std::to_string(static_cast<long long>(cfg.rwTimeoutMs) * 1000).c_str(), 0);
            ret = avio_open2(&m_out->pb, cfg.url.c_str(), AVIO_FLAG_WRITE,
                             &m_out->interrupt_callback, &opts);
            if (opts) av_dict_free(&opts);
            if (ret < 0) {
                logWarn() << "[muxer] 打不开输出" << cfg.url.c_str() << ":" << errStr(ret).c_str();
                return false;
            }
        }

        logInfo().nospace() << "[muxer] 输出已打开: " << cfg.url.c_str()
                          << "  容器=" << m_out->oformat->name
                          << "  rw_timeout=" << cfg.rwTimeoutMs << "ms";
        return true;
    }

    void close()
    {
        if (m_out) {
            if (m_out->pb && !(m_out->oformat->flags & AVFMT_NOFILE))
                avio_closep(&m_out->pb);          // 关闭 IO（会 flush 剩余缓冲）
            avformat_free_context(m_out);
            m_out = nullptr;
        }
        m_inTimeBases.clear();
        m_wroteHeader = false;
        m_wroteTrailer = false;
        m_broken = false;      // 复位：这样同一个对象也能重新 open 再用一次
    }

    AVStream* addStream(AVCodecParameters* par, AVRational timeBase) override
    {
        if (!m_out || !par) return nullptr;

        AVStream* st = avformat_new_stream(m_out, nullptr);
        if (!st) return nullptr;

        // 把编码参数复制进流（注意是 copy，不是接管 par 的所有权）
        int ret = avcodec_parameters_copy(st->codecpar, par);
        if (ret < 0) { logWarn() << "[muxer] avcodec_parameters_copy 失败:" << errStr(ret).c_str(); return nullptr; }
        st->codecpar->codec_tag = 0;    // 交给 muxer 自己决定 tag（mp4/flv 都需要）

        // 先按"输入的时间基"设一个初值；write_header 之后 muxer 可能改写它
        st->time_base = timeBase;

        // [重点] 记住进来的包用的是什么时间基，写包时要拿它做换算
        m_inTimeBases.push_back(timeBase);

        logInfo().nospace() << "[muxer] 已建流 #" << (m_inTimeBases.size() - 1)
                          << " codec=" << avcodec_get_name(par->codec_id)
                          << " 输入时间基=" << timeBase.num << "/" << timeBase.den;
        return st;
    }

    bool writeHeader() override
    {
        if (!m_out) return false;
        int ret = avformat_write_header(m_out, nullptr);
        if (ret < 0) {
            logWarn() << "[muxer] avformat_write_header 失败:" << errStr(ret).c_str();
            return false;
        }
        m_wroteHeader = true;

        // 打印 header 之后每个流"真正"使用的时间基 —— 排查时间戳问题就看这一行
        for (unsigned i = 0; i < m_out->nb_streams; ++i) {
            const AVStream* st = m_out->streams[i];
            logInfo().nospace() << "[muxer] 流 #" << i << " 输出时间基="
                              << st->time_base.num << "/" << st->time_base.den
                              << "（输入是 " << m_inTimeBases[i].num << "/" << m_inTimeBases[i].den << "）";
        }
        return true;
    }

    bool writePacket(AVPacket* pkt) override
    {
        if (!m_out || !m_wroteHeader || !pkt) return false;
        if (pkt->stream_index < 0 || static_cast<size_t>(pkt->stream_index) >= m_inTimeBases.size()) {
            logWarn() << "[muxer] 包里的 stream_index 越界:" << pkt->stream_index;
            return false;
        }

        AVStream* st = m_out->streams[pkt->stream_index];

        // [重点] 时间基换算：编码器时间基 → 输出流时间基（现查，因为 header 之后可能被改写）
        av_packet_rescale_ts(pkt, m_inTimeBases[pkt->stream_index], st->time_base);
        pkt->pos = -1;      // 位置信息对 muxer 没意义，置 -1 避免它去 seek

        // av_interleaved_write_frame 会按 dts 排序并在内部缓冲（多流时保证交织正确），
        // 写完它接管并 unref 这个包 —— 所以调用方不要再 unref 一次。
        const int ret = av_interleaved_write_frame(m_out, pkt);
        if (ret < 0) {
            // 写失败 = 连接坏了。这里只做标记，收拾残局交给上层：
            // 推流线程看到 isOpen() 变 false 之后，会丢掉这个 muxer、重新建一个新的（顺带完成重连）。
            m_broken = true;
            logWarn() << "[muxer] av_interleaved_write_frame 失败:" << errStr(ret).c_str();
            return false;
        }
        ++m_packetsWritten;
        return true;
    }

    void writeTrailer() override
    {
        if (!m_out || !m_wroteHeader || m_wroteTrailer) return;
        // 写已经坏过（比如连接被重置）：这时候再写 trailer 只会又报一次错，直接跳过。
        // （注意 abort() 不算"坏"：abort 只是打断卡住的数据写，trailer 仍然要尽力写出去。）
        if (m_broken) { m_wroteTrailer = true; return; }
        // 撤销可能已经设上的"叫停"标志：abort() 是为了打断卡住的数据写，
        // 而 trailer 是收尾动作（mp4 的 moov 就在里面），必须让它认真写完 ——
        // 否则文件会直接打不开（这也是真实踩过的坑）。
        if (m_abortFlag) m_abortFlag->store(false);
        const int ret = av_write_trailer(m_out);      // mp4 的 moov 箱就在这里写出去
        m_wroteTrailer = true;
        if (ret < 0)
            logWarn() << "[muxer] av_write_trailer 失败:" << errStr(ret).c_str();
        else
            logInfo().nospace() << "[muxer] trailer 已写，共写入 " << m_packetsWritten << " 个包";
    }

    // 输出还活着吗？推流线程靠它决定"继续写"还是"重连"。
    bool isOpen() const override { return m_out && m_wroteHeader && !m_broken; }

private:
    OutputConfig             m_cfg;
    AVFormatContext*         m_out = nullptr;
    std::vector<AVRational>  m_inTimeBases;     // 每个流"输入"的时间基
    std::atomic<bool>*       m_abortFlag = nullptr;   // 借自编排层的中断标志
    std::atomic<bool>        m_abortSelf{ false };    // 没借到时的兜底
    bool                     m_wroteHeader = false;
    bool                     m_wroteTrailer = false;
    bool                     m_broken = false;  // 写过一包失败（连接坏了），需要上层重建
    uint64_t                 m_packetsWritten = 0;
};

} // namespace

std::unique_ptr<IMuxer> createMuxer(const OutputConfig& cfg)
{
    // 以后加 SRT / HTTP-FLV / RTSP 输出，也都在这里按 cfg.format 分支
    (void)cfg;
    return std::make_unique<FormatMuxer>();
}
