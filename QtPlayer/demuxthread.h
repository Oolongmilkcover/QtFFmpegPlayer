#ifndef DEMUXTHREAD_H
#define DEMUXTHREAD_H
/*
打开文件
读 AVPacket
分给 VideoThread、AudioThread
获取：宽、高、帧率、总时长
Seek 功能 
*/
#include <QThread>
class AVFormatContext;
class AVDictionary;
class VideoThread;
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

    //关闭线程清理资源
    void close();
    void clear();
    void closeAVThread();

    //取出pkt,空间需要调用者释放，释放AVPacket对象空间，和数据空间 av_packet_free
    AVPacket* readPkt();
    
    void run() override;
signals:
    
    
private:
    //pts
    long long pts = 0;
    //是否暂停
    bool m_isPause = false;
    //是否退出
    bool m_isExit = false;
    //解封装上下文
    AVFormatContext* m_fmt_ctx = nullptr;
    //配置
    AVDictionary* m_option = nullptr;
    //音视频线程
    VideoThread *m_videoThread = 0;
    AudioThread *m_audioThread = 0;
    //总时长ms
    long long m_totalMs = 0;
    //音视频流
    int m_videoStream = -1;
    int m_audioStream = -1;
    //锁
    std::mutex m_mutex;
    //完成初始化了
    bool isCompleteInit = false;

};

#endif // DEMUXTHREAD_H
