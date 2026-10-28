#ifndef PUSHER_PUSHER_H
#define PUSHER_PUSHER_H

/*
 * 推流端主控（mini-OBS 的编排层）
 *
 * 线程模型 —— 和播放器镜像对称，但背压方向相反：
 *
 *   [视频采集线程] ──AVFrame──► 帧队列(满了丢最旧) ──► [编码线程] ──AVPacket──► 包队列 ──► [写出线程]
 *   [音频采集线程] ──AVFrame──► （就地编码，不走队列）──AVPacket──► 包队列 ──► (同一个写出线程)
 *
 * 三条硬约定（写的时候别破）：
 *   1. 采集是"实时源"：帧队列满了只能丢最旧，绝不能阻塞采集（否则延迟越堆越大）；
 *      包队列相反 —— 落盘时阻塞（文件要完整），推流时才改成丢包（网络卡住不能堆延迟）；
 *   2. 跨线程共享只用"归属明确的队列" + 原子量；每个 AVFormatContext/AVCodecContext 只被一个线程碰；
 *   3. 停止顺序（stop() 里实现）：停采集 → 关帧队列 → 编码线程 flush → 关包队列 → 写完剩余 → writeTrailer。
 *
 * 音频为什么绕过帧队列（这是刻意的，不是偷懒）：
 *   视频丢一帧只是画面抖一下；音频丢一段是"咔哒"一声，体感差得多。
 *   而麦克风本身是带缓冲的实时源，编码器偶尔慢一点它会自然反压，所以音频就地编码、不丢。
 *
 * 里程碑（全部完成）：
 *   [0] --list-devices 列设备
 *   [1] 视频采集 + 观测（打印实际参数 / 落一帧 PPM 看图）
 *   [2] 编码(x264) + 封装落盘 mp4 / flv
 *   [3] RTMP 推流 + 断线重连（退避重连，恢复后等关键帧）
 *   [4] 音频（麦克风/合成正弦 → AAC → 同一个容器）
 *   [5] 参数实验开关（gop / B 帧 / zerolatency / CBR）+ 统计 CSV
 */
#include "capturesource.h"
#include "dropqueue.h"
#include "encoder.h"
#include "muxer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

struct PusherConfig
{
    VideoSourceConfig video;
    AudioSourceConfig audio;

    EncoderConfig encoder;     // 打开采集源后，会用实际参数回填 width/height/fps 与音频三项
    OutputConfig  output;      // url 为空 = 只采集不输出（[第1步] 的观测模式）

    bool withVideo = true;
    bool withAudio = false;    // 默认只推视频；加 --audio 打开

    int  frameQueueCapacity  = 8;    // 采集 → 编码（故意小：攒太多只会加延迟）
    int  packetQueueCapacity = 32;   // 编码 → 写出
    int  durationSeconds     = 0;    // 0 = 一直跑到 Ctrl+C / 收到停止
    std::string dumpFirstFrame;      // 非空则把第一帧存成 PPM（没有预览窗口时的"肉眼验收"）

    // ---- 断线重连 ----
    int reconnectMaxAttempts = 0;      // 0 = 一直重试（直播推流的常规选择）
    int reconnectDelayMs     = 1000;   // 首次重试等待；之后指数退避，上限 15 秒
};

struct PusherStats
{
    std::atomic<uint64_t> videoFrames{0};    // 采集到的视频帧
    std::atomic<uint64_t> audioFrames{0};    // 采集到的音频帧
    std::atomic<uint64_t> encoded{0};        // 视频编码成功的帧数
    std::atomic<uint64_t> audioEncoded{0};   // 音频编码成功的帧数
    std::atomic<uint64_t> sent{0};           // 已写出的包（视频+音频）
    std::atomic<uint64_t> bytes{0};          // 已写出的字节数
    std::atomic<uint64_t> reconnects{0};     // 重连次数
    // 丢帧 / 丢包的计数放在队列自身（DropQueue::dropped()），这里不重复记一份
};

// 队列类型别名，写起来短一点
using PusherFrameQueue  = DropQueue<AVFrame*, AvFrameDeleter>;
using PusherPacketQueue = DropQueue<AVPacket*, AvPacketDeleter>;

class Pusher
{
public:
    explicit Pusher(PusherConfig cfg);
    ~Pusher();

    Pusher(const Pusher&) = delete;
    Pusher& operator=(const Pusher&) = delete;

    bool start();                       // 打开采集源/编码器/输出，起所有线程
    void stop();                        // 有序停止（见头部的停止顺序）
    bool isRunning() const { return m_running.load(); }

    const PusherStats& stats() const { return m_stats; }
    void dumpStatsLine();               // 打一行统计（每秒调一次）

    // 给 --stats-csv 用：一行 CSV（不含换行）
    std::string statsCsvRow(double elapsedSeconds, double cpuPercent) const;

    // 打开之后才知道的"实际"参数
    int actualWidth()  const { return m_video ? m_video->width()  : 0; }
    int actualHeight() const { return m_video ? m_video->height() : 0; }
    int actualFps()    const { return m_video ? m_video->fps()    : 0; }
    int actualSampleRate() const { return m_audio ? m_audio->sampleRate() : 0; }
    int actualChannels()   const { return m_audio ? m_audio->channels()   : 0; }

private:
    void videoCaptureLoop();            // 读帧 → 帧队列（或直接释放）
    void audioCaptureLoop();            // 读 PCM → 就地编码 → 包队列
    void encodeLoop();                  // 帧队列 → 视频编码 → 包队列；退出前 flush
    void pushLoop();                    // 包队列 → muxer；失败时重连；结束时写 trailer

    bool addOutputStreams(IMuxer& muxer);   // 把视频/音频流加进 muxer（首次和重连共用）
    bool ensureOutputReady();               // 只在写出线程调用：输出坏了就地重连

    PusherConfig m_cfg;
    PusherStats  m_stats;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_waitingKeyframe{ false };   // 重连之后要等一个关键帧才能继续写
    std::atomic<bool> m_outputGivenUp{ false };     // 重连次数用尽，不再尝试
    std::atomic<bool> m_pushIdle{ false };          // 写出线程已经收工（stop 用它决定要不要"叫停"）
    bool m_encodePath = false;          // 配置里给了 output.url 才走编码/封装这条路

    std::unique_ptr<IVideoSource> m_video;
    std::unique_ptr<IAudioSource> m_audio;

    std::unique_ptr<IVideoEncoder> m_videoEnc;
    std::unique_ptr<IAudioEncoder> m_audioEnc;
    // 流下标由编排层分配（addOutputStreams 里按实际加入顺序记录），编码器不再写死 0/1。
    // 这样以后加第三路流（比如录屏、第二路摄像头）时才不会悄悄串流。
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;
    // 用 shared_ptr + 原子读写：stop() 需要在"写出线程可能正在换掉它"的情况下安全地叫停它。
    // 若是裸指针，就成了一个真实的数据竞争（可能 use-after-free）。
    std::shared_ptr<IMuxer> m_muxer;

    std::unique_ptr<PusherFrameQueue>  m_frameQueue;
    std::unique_ptr<PusherPacketQueue> m_packetQueue;

    std::thread m_videoThread;
    std::thread m_audioThread;
    std::thread m_encodeThread;
    std::thread m_pushThread;

    bool m_dumpedFirstFrame = false;
};

#endif // PUSHER_PUSHER_H
