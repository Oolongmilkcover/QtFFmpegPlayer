#ifndef DEMUXTHREAD_H
#define DEMUXTHREAD_H
/*
打开文件
读 AVPacket
分给 VideoDecodeThread、AudioThread
获取：宽、高、帧率、总时长
Seek 功能 
*/

#include "libavutil/rational.h"
#include <QSet>
#include <QThread>
class AVFormatContext;
class AVDictionary;
class VideoDecodeThread;
class AudioThread;
class VideoWidget;
class AVPacket;
class DemuxThread : public QThread
{
    Q_OBJECT
public:
    explicit DemuxThread(QObject *parent = nullptr);
    ~DemuxThread();
    //打开文件
    bool openFile(const char* url,VideoWidget* widget);
    
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
    bool isFrameStep() const { return m_isFrameStep.load(); }


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

    void run() override;

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
    bool hasVideo() const { return m_hasVideo; }

private slots:
    void setDone();

    void videoCallSeek(long long ms);
signals:
    void disableBtn();
    void ableBtn();
    // void moveSlider(long long pts);
    void playNext();
    void needPause();
private:
    AVRational m_audioTimebase{};
    AVRational m_videoTimebase{};

    //普通seek流程，由run()调用
    void doSeek();
    //发起一次以毫秒为目标的seek（seek / seekToMs / 逐帧对齐都走这里）
    bool requestSeekMs(long long ms);
    //逐帧回退到历史最旧帧后，向后解码一段补进历史
    void doBackwardRefill();

    // 上一次没推进包队列的包（队列满），下次循环重试
    AVPacket* m_pendingPkt = nullptr;

    //是否暂停
    std::atomic<bool> m_isPause = false;
    //是否退出
    std::atomic<bool>  m_isExit = false;
    //解封装上下文
    AVFormatContext* m_fmt_ctx = nullptr;
    //配置
    AVDictionary* m_option = nullptr;
    //音视频线程
    VideoDecodeThread *m_videoDecodeThread = nullptr;
    AudioThread *m_audioThread = nullptr;

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

    //异步seek
    std::atomic<bool> m_isSeeking = false;
    //seek目标（毫秒）
    std::atomic<long long> m_seekMs = 0;

    std::atomic<bool> m_eof = false;

    //seek的serial
    std::atomic<int> m_serial = 0;

    //主程序是否有播放列表
    std::atomic<bool> m_hasPlayList = false;

    //是否在逐帧
    std::atomic<bool> m_isFrameStep = false;
    //进入逐帧之前的暂停状态（退出逐帧时恢复）
    std::atomic<bool> m_pauseBeforeStep = false;
    //正在处理的 seek 结束后应该恢复成什么暂停状态
    std::atomic<bool> m_pauseAfterSeek = false;

    // 注：这里原先有 m_containerName / m_disableSeekFlag / m_audioOnlyFormat，
    // 用「封装名是否在纯音频集合里」判断纯音频，并据此在调用者（GUI）线程里直接
    // 调 av_seek_frame。该做法有两个问题：
    //   1) 容器名会漏判：纯音频 .m4a 的 iformat->name 是 "mov,mp4,m4a,3gp,3g2,mj2"，
    //      并不等于集合里的 "m4a"；而 !m_hasVideo 对所有纯音频文件都成立；
    //   2) 在调用者线程里 seek 会与 demux 线程的 av_read_frame 并发操作同一个
    //      AVFormatContext（readPkt() 并不持 m_mutex），属于数据竞争。
    // 现在纯音频统一用 !m_hasVideo 判断，所有 seek 一律走 demux 线程里的 doSeek()。

    std::atomic<double> m_speed{1.0};
};

#endif // DEMUXTHREAD_H
