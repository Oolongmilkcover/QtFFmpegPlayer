#include "pusher.h"

#include "log.h"

extern "C" {
#include <libavutil/pixdesc.h>     // av_get_pix_fmt_name
#include <libavutil/samplefmt.h>   // av_get_sample_fmt_name
}

#include <chrono>
#include <cstdio>

namespace {

// 输出是"网络推流"还是"本地文件"？
// 这决定包队列满了怎么办：网络流必须丢包（不能堆延迟），文件必须阻塞等待（不能缺数据）。
bool isNetworkOutput(const OutputConfig& out)
{
    if (out.format == "flv" || out.format == "rtmp") return true;
    return out.url.rfind("rtmp", 0) == 0
        || out.url.rfind("rtsp", 0) == 0
        || out.url.rfind("srt",  0) == 0;
}

} // namespace

// ============================================================================================
// 生命周期
// ============================================================================================

Pusher::Pusher(PusherConfig cfg)
    : m_cfg(std::move(cfg))
{
    // 队列在构造时就建好（容量和策略都来自配置）

    // 帧队列：采集是实时源 → 必须丢最旧，绝不能让采集线程等。
    // 容量故意小（默认 8）：攒太多只会让"采集 → 编码"之间的延迟变大，对实时链路是坏事。
    m_frameQueue = std::make_unique<PusherFrameQueue>(
        static_cast<size_t>(m_cfg.frameQueueCapacity), AvFrameDeleter{},
        /*dropOldestWhenFull=*/true);

    // 包队列：推流时丢包；落盘时阻塞等待
    m_packetQueue = std::make_unique<PusherPacketQueue>(
        static_cast<size_t>(m_cfg.packetQueueCapacity), AvPacketDeleter{},
        isNetworkOutput(m_cfg.output));
}

Pusher::~Pusher()
{
    stop();
}

bool Pusher::start()
{
    if (m_running.load())
        return true;

    // 给了 --output/--url 才走"编码 + 封装"这条路；否则是 [第1步] 的纯采集观测模式
    m_encodePath = !m_cfg.output.url.empty();

    // ---------------------------------------------------------------- 视频源
    if (m_cfg.withVideo) {
        m_video = createVideoSource(m_cfg.video);
        if (!m_video || !m_video->open(m_cfg.video)) {
            logErr() << "[pusher] 视频源打开失败；没有摄像头可以用 OBS 虚拟摄像头"
                           "（--video \"video=OBS Virtual Camera\"）或内置合成源（--testsrc）";
            return false;
        }
        // 用设备实际给的值回填编码器配置：编码器与输入必须完全一致
        m_cfg.encoder.width  = m_video->width();
        m_cfg.encoder.height = m_video->height();
        m_cfg.encoder.fps    = m_video->fps();
        // yuv420p / x264 要求偶数宽高；设备端如果给的是奇数（少见但可能），提前说清楚
        if (m_cfg.encoder.width % 2 || m_cfg.encoder.height % 2)
            logWarn() << "[pusher] 设备给出的分辨率是奇数" << m_cfg.encoder.width << "x"
                       << m_cfg.encoder.height << "，yuv420p 编码可能失败，请在设备端改成偶数分辨率";
    }

    // ---------------------------------------------------------------- 音频源
    if (m_cfg.withAudio) {
        m_audio = createAudioSource(m_cfg.audio);
        if (!m_audio || !m_audio->open(m_cfg.audio)) {
            // 音频是可选能力：失败不整体退出，退化成"只推视频"更容易排查
            logWarn() << "[pusher] 音频源打开失败，本次只采集视频";
            m_audio.reset();
        } else {
            // 同样用实际值回填（AAC 编码器内部会用 swresample 转成它要的格式）
            m_cfg.encoder.audioInSampleRate = m_audio->sampleRate();
            m_cfg.encoder.audioInChannels   = m_audio->channels();
            m_cfg.encoder.audioInSampleFmt  = m_audio->sampleFormat();
        }
    }

    // ---------------------------------------------------------------- 编码 + 封装
    if (m_encodePath) {
        // ---- 视频编码器 ----
        m_videoEnc = createVideoEncoder(m_cfg.encoder);
        if (!m_videoEnc || !m_videoEnc->open(m_cfg.encoder)) {
            logErr() << "[pusher] 视频编码器打开失败";
            return false;
        }
        // 编码器每产出一包就回调到这里。回调给的指针只在回调期间有效，所以必须 clone：
        // av_packet_clone 只给码流数据加一次引用（不拷贝码流），队列最终负责 av_packet_free。
        // [重点] 流下标在这里赋：编码器自己不知道"我是第几路"，由编排层按建流顺序分配。
        //        sink 是在每次回调时才读 m_videoStreamIndex，而它早在起线程之前就设好了，所以顺序没问题。
        m_videoEnc->setSink([this](AVPacket* pkt) {
            AVPacket* copy = av_packet_clone(pkt);
            if (!copy) return;
            copy->stream_index = m_videoStreamIndex;
            m_packetQueue->push(copy);
        });

        // ---- 音频编码器（[第4步]）----
        if (m_audio) {
            m_audioEnc = createAudioEncoder(m_cfg.encoder);
            if (m_audioEnc && m_audioEnc->open(m_cfg.encoder)) {
                m_audioEnc->setSink([this](AVPacket* pkt) {
                    AVPacket* copy = av_packet_clone(pkt);
                    if (!copy) return;
                    copy->stream_index = m_audioStreamIndex;
                    m_packetQueue->push(copy);
                });
            } else {
                logWarn() << "[pusher] 音频编码器打开失败，本次只推视频（音频仍会采集和统计）";
                m_audioEnc.reset();
            }
        }

        // ---- 输出（文件 或 RTMP）----
        std::unique_ptr<IMuxer> muxer = createMuxer(m_cfg.output);
        if (!muxer || !muxer->open(m_cfg.output, &m_abortIo)) {
            logErr() << "[pusher] 打开输出失败:" << m_cfg.output.url.c_str();
            return false;
        }
        if (!addOutputStreams(*muxer)) {
            logErr() << "[pusher] 建输出流失败";
            return false;
        }
        if (!muxer->writeHeader()) {
            logErr() << "[pusher] 写容器头失败";
            return false;
        }
        m_muxer = std::move(muxer);
    }

    // ---------------------------------------------------------------- 起线程
    m_running.store(true);
    if (m_video) m_videoThread = std::thread(&Pusher::videoCaptureLoop, this);
    if (m_audio) m_audioThread = std::thread(&Pusher::audioCaptureLoop, this);
    if (m_encodePath) {
        m_encodeThread = std::thread(&Pusher::encodeLoop, this);
        m_pushThread   = std::thread(&Pusher::pushLoop,   this);
    }
    return true;
}

void Pusher::stop()
{
    if (!m_running.exchange(false))
        return;

    /*
     * 停止顺序（头文件里的第 3 条硬约定，顺序不能换）：
     *   1) 请求采集源停止：采集线程多半正阻塞在 av_read_frame，靠中断回调把它踢出来；
     *   2) join 采集线程：此后不会再有新帧（音频线程退出前会 flush 音频编码器）；
     *   3) 关帧队列：编码线程把剩下的帧编完（pop 返回 false）后退出，退出前 flush 视频编码器，
     *      吐出来的包进包队列；
     *   4) 关包队列：写出线程把队列里剩下的包全部写完再退出；
     *   5) join 写出线程：trailer 由它自己写（它是这个 muxer 的唯一使用者）；
     *   6) 关采集源。
     */
    if (m_video) m_video->requestStop();
    if (m_audio) m_audio->requestStop();

    if (m_videoThread.joinable()) m_videoThread.join();
    if (m_audioThread.joinable()) m_audioThread.join();

    m_frameQueue->close();
    if (m_encodeThread.joinable()) m_encodeThread.join();

    m_packetQueue->close();

    /*
     * 关掉包队列之后，写出线程会把队列里剩下的数据写完、然后自己写 trailer。
     * 这里要区分两种情况：
     *   - 落盘：写文件不会无限期卡住，所以绝不能叫停 —— 一叫停，剩下的包和 trailer 全废
     *     （曾经踩过：无条件 abort() 之后 av_write_trailer 报 "Immediate exit requested"，
     *      mp4 里没有 moov，文件直接打不开）；
     *   - 推流：网络可能真的卡死，先给它 2 秒把剩余数据送出去，还不行再叫停
     *     （rw_timeout 是第二道保险）。
     */
    if (isNetworkOutput(m_cfg.output)) {
        for (int waited = 0; waited < 2000 && !m_pushIdle.load(); waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!m_pushIdle.load()) {
            {
                logWarn() << "[pusher] 写出线程没有在 2 秒内收工，主动叫停网络写（剩余数据可能丢失）";
                m_abortIo.store(true);   // 只置标志，不碰 m_muxer（那个指针归写出线程）
            }
        }
    }

    if (m_pushThread.joinable()) m_pushThread.join();

    if (m_video) m_video->close();
    if (m_audio) m_audio->close();

    dumpStatsLine();   // 收尾再打一次，方便对账
}

// ============================================================================================
// 输出：建流 / 重连
// ============================================================================================

bool Pusher::addOutputStreams(IMuxer& muxer)
{
    // [重点] 流下标在这里定下来（不再由编码器写死）：
    //   muxer 的 addStream 顺序决定流的编号，编码器产出的包必须带对应的 stream_index，
    //   否则 muxer 会用错时间基，甚至把音频写进视频流。
    int next = 0;
    if (m_videoEnc) {
        if (!muxer.addStream(m_videoEnc->codecpar(), m_videoEnc->timeBase())) return false;
        m_videoStreamIndex = next++;
    }
    if (m_audioEnc) {
        if (!muxer.addStream(m_audioEnc->codecpar(), m_audioEnc->timeBase())) return false;
        m_audioStreamIndex = next++;
    }
    return true;
}

/*
 * (重)连输出 —— 只在写出线程里调用。
 *
 * 重连之后为什么必须"等关键帧"：
 *   H.264 的 P 帧要参考前面的帧，而服务端/播放器在断点处没有参考帧。
 *   硬接着写会让画面花一大片；等到下一个 IDR（关键帧）重新开始最干净。
 *   等待期间音频包也一起丢，这样音画不会错位。
 *
 * 时间戳：重连后继续用原来的时间轴（单调递增、不回到 0）。
 *   好处是实现简单、不会出现"时间倒退"；代价是观众看到的时间戳在断点处是跳着的。
 *   真正上生产可以在这里做"重连后时间戳归零"，那属于产品策略，不是这里的技术问题。
 */
bool Pusher::ensureOutputReady()
{
    // 快路径：每个包都会走到这里，所以只做最少判断
    {
        IMuxer* cur = m_muxer.get();
        if (cur && cur->isOpen()) return true;
    }
    if (m_outputGivenUp.load()) return false;

    // 旧的已经坏了：先收掉它
    {
        if (m_muxer) {
            m_abortIo.store(true);              // 万一还卡在网络写里，置标志叫停它
            m_muxer.reset();
            m_waitingKeyframe.store(true);      // 恢复后要等关键帧
            m_stats.reconnects.fetch_add(1);
        }
    }

    int delayMs = m_cfg.reconnectDelayMs > 0 ? m_cfg.reconnectDelayMs : 1000;

    for (int attempt = 1; m_running.load(); ++attempt) {
        if (m_cfg.reconnectMaxAttempts > 0 && attempt > m_cfg.reconnectMaxAttempts) {
            logErr() << "[pusher] 重连达到上限" << m_cfg.reconnectMaxAttempts
                        << "次，放弃输出（后续的包全部丢弃，但采集还在继续）";
            m_outputGivenUp.store(true);
            return false;
        }

        logInfo().nospace() << "[pusher] 连接输出（第 " << attempt << " 次）: "
                          << m_cfg.output.url.c_str();

        std::unique_ptr<IMuxer> muxer = createMuxer(m_cfg.output);
        if (muxer && muxer->open(m_cfg.output, &m_abortIo) && addOutputStreams(*muxer) && muxer->writeHeader()) {
            m_muxer = std::move(muxer);
            logInfo() << "[pusher] 输出已恢复；等下一个关键帧再继续写";
            return true;
        }

        // 100ms 一粒地睡：这样收工时能立刻退出，不用等完整个退避周期
        logWarn().nospace() << "[pusher] 连接失败，" << delayMs << "ms 后重试";
        for (int slept = 0; slept < delayMs && m_running.load(); slept += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        delayMs *= 2;                            // 指数退避
        if (delayMs > 15000) delayMs = 15000;    // 上限 15 秒
    }
    return false;
}

// ============================================================================================
// 采集线程
// ============================================================================================

void Pusher::videoCaptureLoop()
{
    logInfo() << "[pusher] 视频采集线程启动";

    while (m_running.load()) {
        AVFrame* frame = m_video->readFrame();      // 阻塞读（可被 requestStop 打断）
        if (!frame) {
            if (m_running.load())
                logWarn() << "[pusher] 视频读帧返回空，采集线程结束";
            break;
        }

        const uint64_t n = m_stats.videoFrames.fetch_add(1) + 1;

        if (n == 1) {
            logInfo().nospace() << "[pusher] 第 1 帧到达: " << frame->width << "x" << frame->height
                              << " format=" << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format))
                              << " pts=" << frame->pts << "us"
                              << " linesize=[" << frame->linesize[0] << "," << frame->linesize[1] << "]";
        }

        // 没有预览窗口时的"肉眼验收"：把第一帧存成 PPM
        if (!m_dumpedFirstFrame && !m_cfg.dumpFirstFrame.empty()) {
            saveFrameAsPpm(frame, m_cfg.dumpFirstFrame);
            m_dumpedFirstFrame = true;
        }

        const int fps = m_video->fps() > 0 ? m_video->fps() : 30;
        if (n % static_cast<uint64_t>(fps) == 0) {
            logInfo().nospace() << "[pusher] 已采集 " << n << " 帧 (约 "
                              << (n / static_cast<uint64_t>(fps)) << " 秒)";
        }

        if (m_encodePath) {
            // 交棒给编码线程：所有权转给队列；队列满时它会丢最旧的（并计数）
            m_frameQueue->push(frame);
        } else {
            av_frame_free(&frame);      // 纯采集观测模式：就地释放
        }
    }

    logInfo().nospace() << "[pusher] 视频采集线程退出，共采集 " << m_stats.videoFrames.load() << " 帧";
}

/*
 * 音频：采集 → 就地编码 → 包队列。
 *
 * 为什么不走"帧队列 + 丢帧"那一套：视频丢一帧只是画面抖一下，音频丢一段是"咔哒"声，
 * 体感差得多。而麦克风本身是带缓冲的实时源，编码器偶尔慢一点它会自然反压。
 * 所以音频这条路是"不丢"的（代价是极端情况下音频线程会稍微落后，而那正是我们想要的取舍）。
 */
void Pusher::audioCaptureLoop()
{
    logInfo() << "[pusher] 音频采集线程启动";

    while (m_running.load()) {
        AVFrame* frame = m_audio->readFrame();
        if (!frame) {
            if (m_running.load())
                logWarn() << "[pusher] 音频读帧返回空，采集线程结束";
            break;
        }

        const uint64_t n = m_stats.audioFrames.fetch_add(1) + 1;
        if (n == 1) {
            logInfo().nospace() << "[pusher] 第 1 个音频帧: "
                              << frame->sample_rate << "Hz "
                              << frame->ch_layout.nb_channels << "声道 "
                              << av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format))
                              << " nb_samples=" << frame->nb_samples
                              << " pts=" << frame->pts << "us";
        }
        // 音频帧很密（每秒几十个），少打点日志
        if (n % 200 == 0)
            logInfo().nospace() << "[pusher] 已采集音频 " << n << " 帧";

        if (m_audioEnc && m_audioEnc->encode(frame))
            m_stats.audioEncoded.fetch_add(1);

        av_frame_free(&frame);
    }

    // 收尾：把 swresample / FIFO 里剩下的样本编出去（否则结尾会丢掉一小段声音）
    if (m_audioEnc) m_audioEnc->flush();

    logInfo().nospace() << "[pusher] 音频采集线程退出，共采集 " << m_stats.audioFrames.load()
                      << " 帧、编码 " << m_stats.audioEncoded.load() << " 帧";
}

// ============================================================================================
// 视频编码线程 / 写出线程
// ============================================================================================

void Pusher::encodeLoop()
{
    logInfo() << "[pusher] 编码线程启动";

    AVFrame* frame = nullptr;
    while (true) {
        // 用 100ms 超时轮询而不是"死等"：关队列时能及时退出，
        // 同时不会因为一次超时就误判结束（要看 closed()）。
        if (!m_frameQueue->pop(frame, 100)) {
            if (m_frameQueue->closed()) break;      // 上游收工且队列已空
            continue;
        }

        if (m_videoEnc && m_videoEnc->encode(frame))
            m_stats.encoded.fetch_add(1);

        av_frame_free(&frame);                      // 队列交回给我们的所有权
    }

    // 收尾：把编码器内部压着的包全部吐出来（可能有好几帧的量）。
    // 这些包会经 sink 进入包队列，所以必须在包队列 close 之前执行。
    if (m_videoEnc) m_videoEnc->flush();

    logInfo().nospace() << "[pusher] 编码线程退出，共编码 " << m_stats.encoded.load() << " 帧";
}

void Pusher::pushLoop()
{
    logInfo() << "[pusher] 写出线程启动";

    AVPacket* pkt = nullptr;
    while (true) {
        if (!m_packetQueue->pop(pkt, 100)) {
            if (m_packetQueue->closed()) break;     // 上游收工且队列已空
            continue;
        }

        // 输出可用吗？坏了就地重连；重连期间这些包直接丢掉 —— 这就是"实时优先"的取舍
        if (!ensureOutputReady()) {
            av_packet_free(&pkt);
            pkt = nullptr;
            continue;
        }

        // 重连之后先丢到下一个关键帧（原因见 ensureOutputReady 的注释）
        // 注意：只认视频关键帧。音频包（AAC）本身没有"参考帧"的概念，
        // 有些实现会给音频包打上 AV_PKT_FLAG_KEY，那样会把等待状态错误地清掉，
        // 结果从一个 P 帧开始写、画面花一段时间。
        if (m_waitingKeyframe.load()) {
            const bool isVideoKey = (pkt->stream_index == 0) && (pkt->flags & AV_PKT_FLAG_KEY);
            if (!isVideoKey) {
                av_packet_free(&pkt);
                pkt = nullptr;
                continue;
            }
            m_waitingKeyframe.store(false);
            logInfo() << "[pusher] 收到视频关键帧，恢复写出";
        }

        const int pktSize = pkt->size;      // [重点] 先记下来：写完之后包会被 unref，size 会变 0
        IMuxer* muxer = m_muxer.get();
        if (muxer && muxer->writePacket(pkt)) {
            m_stats.sent.fetch_add(1);
            m_stats.bytes.fetch_add(static_cast<uint64_t>(pktSize));
        }
        av_packet_free(&pkt);
        pkt = nullptr;
    }

    // 收尾：trailer 必须写（mp4 的 moov 箱就在这里，不写文件基本废掉）。
    // 由写出线程自己做，因为它是这个 muxer 的唯一使用者。
    if (m_muxer) {
        m_muxer->writeTrailer();
        m_muxer.reset();   // 析构里会关闭 IO
    }

    // 告诉 stop()："我已经收工了" —— 它据此决定要不要叫停网络写
    m_pushIdle.store(true);

    logInfo().nospace() << "[pusher] 写出线程退出，共写出 " << m_stats.sent.load() << " 个包";
}

// ============================================================================================
// 统计
// ============================================================================================

void Pusher::dumpStatsLine()
{
    const uint64_t droppedFrames  = m_frameQueue  ? m_frameQueue->dropped()  : 0;
    const uint64_t droppedPackets = m_packetQueue ? m_packetQueue->dropped() : 0;
    const size_t   queueFrames    = m_frameQueue  ? m_frameQueue->size()     : 0;
    const size_t   queuePackets   = m_packetQueue ? m_packetQueue->size()    : 0;

    logInfo().nospace() << "[stats] 采集视频=" << m_stats.videoFrames.load()
                      << " 音频=" << m_stats.audioFrames.load()
                      << " 编码=" << m_stats.encoded.load()
                      << " 音频编码=" << m_stats.audioEncoded.load()
                      << " 写出=" << m_stats.sent.load()
                      << " 丢帧=" << droppedFrames
                      << " 丢包=" << droppedPackets
                      << " 重连=" << m_stats.reconnects.load()
                      << " 队列(帧/包)=" << queueFrames << "/" << queuePackets
                      << " 已写=" << (m_stats.bytes.load() / 1024 / 1024) << "MB";
}

std::string Pusher::statsCsvRow(double elapsedSeconds, double cpuPercent) const
{
    const uint64_t droppedFrames  = m_frameQueue  ? m_frameQueue->dropped()  : 0;
    const uint64_t droppedPackets = m_packetQueue ? m_packetQueue->dropped() : 0;
    const size_t   queueFrames    = m_frameQueue  ? m_frameQueue->size()     : 0;
    const size_t   queuePackets   = m_packetQueue ? m_packetQueue->size()    : 0;
    const uint64_t bytes          = m_stats.bytes.load();

    // 列和 main.cpp 里写的表头一一对应（改这里记得改那里）
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%.1f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%zu,%zu,%llu,%.1f,%.1f",
                  elapsedSeconds,
                  static_cast<unsigned long long>(m_stats.videoFrames.load()),
                  static_cast<unsigned long long>(m_stats.audioFrames.load()),
                  static_cast<unsigned long long>(m_stats.encoded.load()),
                  static_cast<unsigned long long>(m_stats.audioEncoded.load()),
                  static_cast<unsigned long long>(m_stats.sent.load()),
                  static_cast<unsigned long long>(droppedFrames + droppedPackets),
                  static_cast<unsigned long long>(m_stats.reconnects.load()),
                  queueFrames, queuePackets,
                  static_cast<unsigned long long>(bytes),
                  elapsedSeconds > 0.01 ? (bytes * 8.0 / 1000.0 / elapsedSeconds) : 0.0,
                  cpuPercent);
    return std::string(buf);
}
