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
    void stepNextFrame();
    //回放上一帧
    void stepPrevFrame();


    //跳转
    bool seek(double pos);

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

    double m_saveVolume = 0;

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

private slots:
    void setDone();

signals:
    void disableBtn();
    void ableBtn();
    // void moveSlider(long long pts);
    void playNext();
private:
    AVRational m_audioTimebase{};
    AVRational m_videoTimebase{};

    //是否暂停
    std::atomic<bool> m_isPause = false;
    //上次暂停状态
    std::atomic<bool> m_lastIsPause = false;
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
    //锁
    std::mutex m_mutex;
    //完成初始化了
    std::atomic<bool> isCompleteInit = false;

    //异步seek
    std::atomic<bool> m_isSeeking = false;
    std::atomic<double> m_seekPos = 0;

    std::atomic<bool> m_eof = false;

    //seek的serial
    std::atomic<int> m_serial = 0;

    //主程序是否有播放列表
    std::atomic<bool> m_hasPlayList = false;

    //是否在逐帧
    std::atomic<bool> m_isFrameStep = false;

};

#endif // DEMUXTHREAD_H
