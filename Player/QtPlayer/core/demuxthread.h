#ifndef DEMUXTHREAD_H
#define DEMUXTHREAD_H
/*
打开文件
读 AVPacket
分给 VideoDecodeThread、AudioThread
获取：宽、高、帧率、总时长
Seek 功能 
*/
#include "core/iaudiooutput.h"
#include "core/playercontext.h"
#include "libavutil/rational.h"
#include <set>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>


struct AVFormatContext;
struct AVDictionary;
class VideoDecodeThread;
class AudioThread;
class VideoWidget;
struct AVPacket;
class PlayerContext;
class MessageQueue;
struct AVFrame;
//视频输出回调
using PaintFrame = std::function<void(AVFrame* frame)>;

class DemuxThread
{
public:
    explicit DemuxThread();
    ~DemuxThread();

    //设置与PlayerCore共享的上下文 只允许访问  要修改得返回信息到PlayerCore由他修改
    void setPlayerContext(PlayerContext* ctx);
    //设置上报信息队列
    void setMessageQueue(MessageQueue* msgQue);


    //打开文件
    bool openFile(const char* url,PaintFrame cb , IAudioOutput* out);
    
    //启动所有线程
    void start();
    
    //暂停
    void setPause(bool isPause);

    //开始逐帧
    void startFrameStep();
    //结束逐帧
    void endFrameStep();
    //逐下帧
    bool stepNextFrame();
    //回放上一帧
    bool stepPrevFrame();
    //是否处于逐帧状态
    //bool isFrameStep() const;


    //跳转（pos = 0.0~1.0）
    bool seek(double pos);
    //按毫秒跳转：逐帧对齐用，避免 pos<->ms 来回换算把目标挪到下一帧
    bool seekToMs(long long ms);

    //关闭线程清理资源
    void close();
    void clear();
    void closeAVThread();

    //取出pkt,空间需要调用者释放，释放AVPacket对象空间，和数据空间 av_packet_free
    AVPacket* readPkt();

    //返回isPause
    bool getIsPause();

    //进度条用获取pts
    long long getVideoPts();

    void run() ;

    //pts
    std::atomic<long long >  pts = 0;
    std::atomic<bool> playDone = false;
    //总时长ms
    long long totalMs = 0;

    //宽高
    int m_width = 0;
    int m_height = 0;

    bool getIsExit() const;

    /*
    解决网络流会让退出卡死
    播放器支持 rtsp 流，readPkt() 里的 av_read_frame 对网络流可能阻塞很久，dt.close() 里的 wait() 会一直等 demux 线程 → 窗口关不掉。
    解决：给 FFmpeg 加中断回调（这是播放器支持"随时退出"的标准做法）
    */
    static int interruptCallback(void *opaque)
    {
        DemuxThread *d = static_cast<DemuxThread*>(opaque);
        return d->getIsExit();   // 退出标志
    }

    void setVolume(double& pos);

    void setHasPlayList(bool has);

    // 倍速：同时设置音频 atempo 和视频帧时长
    void setSpeed(double speed);

    //是否有视频流
    //bool hasVideo() const { return m_hasVideo; }

    /*
     * 本次打开的是不是网络流媒体（按 URL 的 "协议://" 判定，见 demuxthread.cpp 的 isStreamUrl）。
     *
     * 流媒体不支持 快进/快退、逐帧、倍速、上下集 —— 这几个机制都建立在
     * "能 seek / 时长确定 / 能重定位到某一帧"之上，直播流一个都不满足。
     * 每次 openFile 按新 URL 重新判定、close() 复位，所以换回本地视频后自动恢复可用。
     */
    //bool isStreamMedia() const { return m_isStreamMedia.load(); }

    std::atomic<bool> m_seekPauseing = false;

private:
    void setDone();

    void videoCallSeek(long long ms);

    void checkTimeToPostPosition();

private:

    //共同上下文
    PlayerContext* m_playerctx = nullptr;
    //上报信息队列
    MessageQueue* m_msgQue = nullptr;

    AVRational m_audioTimebase{};
    AVRational m_videoTimebase{};

    //普通seek流程，由run()调用
    void doSeek();
    //发起一次以毫秒为目标的seek（seek / seekToMs / 逐帧对齐都走这里）
    bool requestSeekMs(long long ms);
    //逐帧回退到历史最旧帧后，向后解码一段补进历史
    void doBackwardRefill();
    //真正把倍速下发到音视频线程（跳过 setSpeed 里"流媒体不支持倍速"的闸门）
    void applySpeed(double speed);

    // 上一次没推进包队列的包（队列满），下次循环重试
    AVPacket* m_pendingPkt = nullptr;

    /*
     * 用户意图：是否暂停。**只有 setPause()（即 UI 的播放/暂停命令）能改它。**
     * doSeek() 期间为停生产只推 CTX_IS_PAUSE=1（ctx），不要改这个值 ——
     * 以前 doSeek 会把它临时置 true 再按开头快照恢复，导致
     *   ① requestSeekMs 采样到"临时暂停"当成用户意图 → seek 结束后卡在暂停；
     *   ② seek 期间用户按的播放/暂停被快照覆盖 → "快进后要按两次播放"。
     * 现在 doSeek 结尾直接读它恢复，同步可见，不会失真。
     */
    std::atomic<bool> m_isPause = false;
    //是否退出
    //DemuxThread 自己的停止标志：close() 直接置它，不依赖 PlayerCore 把
    //CTX_IS_EXIT 消息处理完 —— 否则 close() 里 join 自己这条线程会永远等不到。
    std::atomic<bool> m_isExit{false};
    //解封装上下文
    AVFormatContext* m_fmt_ctx = nullptr;
    //配置
    AVDictionary* m_option = nullptr;
    //音视频线程
    VideoDecodeThread *m_videoDecodeThread = nullptr;
    AudioThread *m_audioThread = nullptr;

    //demux 线程本体
    std::thread m_dumuxThread;

    //run() 及内部循环的退出判断：自己收到停止
    bool shouldExit() const { return m_isExit.load() ; }

    //音视频流
    int m_videoStream = -1;
    int m_audioStream = -1;

    //判断流是否存在
    bool m_hasVideo = false;
    bool m_hasAudio = false;


    //锁
    std::mutex m_mutex;
    //完成初始化了
    std::atomic<bool> isCompleteInit = false;

    //上一次上报 EVT_POSITION 的时刻（只由 demux 线程读写）
    std::chrono::steady_clock::time_point m_lastPosTime = std::chrono::steady_clock::now();


    /*
     * 是否处于逐帧 —— 本类是**唯一**权威状态源。
     *   写：只有 startFrameStep() / endFrameStep() / close() 改它；
     *   读：setPause / requestSeekMs / stepNext|PrevFrame / run / doBackwardRefill
     *       一律读这个，**不要**再读 PlayerContext::getIsFrameStep()。
     * 原因：PlayerContext 那份是发给 UI 的广播副本，要等消息循环处理完
     * CTX_IS_FRAME_STEP 才更新，慢一整个消息往返；拿它做本类内部的判断，
     * 会让 endFrameStep↔setPause 互相递归（详见 .cpp 里 endFrameStep 的注释）。
     */
    std::atomic<bool> m_isFrameStep{false};

    std::atomic<bool> m_eof = false;

    //seek的serial
    std::atomic<int> m_serial = 0;

    //主程序是否有播放列表
    //std::atomic<bool> m_hasPlayList = false;

    //（逐帧状态已上移，见 m_isFrameStep）
    //进入逐帧之前的暂停状态（退出逐帧时恢复）
    std::atomic<bool> m_pauseBeforeStep = false;
    //（seek 结束该恢复成什么，已改成读 m_isPause，不再单独保存快照）

    // 实际解析出来的封装名（std::string，core 不再用 QString）
    std::string m_containerName;
    //当作音频文件符号
    std::atomic<bool> m_disableSeekFlag{false};

    /*
     * 是否网络流媒体（openFile 成功后写入，close 复位）。
     * 流媒体的闸门：requestSeekMs / startFrameStep / stepNext|PrevFrame /
     * setSpeed 都会先看这个标志，另见 isStreamMedia()。
     */
    std::atomic<bool> m_isStreamMedia{false};



    // 纯音频，需要禁用Seek功能 的格式集合
    const std::set<std::string> m_audioOnlyFormat = {
        "mp3",
        "aac",
        "flac",
        "ogg",
        "opus",
        "wav",
        "wma",
        "ape",
        "alac",
        "m4a",
        "ac3",
        "eac3",
        "dts",
        "amr",
        "wv",
        "tta"
    };
};

#endif // DEMUXTHREAD_H
