/*
发送 AVPacket
接收 AVFrame
音频、视频都继承它

*/
#ifndef DECODETHREAD_H
#define DECODETHREAD_H
#include <QThread>
#include<queue>
#include<mutex>
class AVCodec;
class AVCodecContext;
class AVPacket;
struct AVFrame;
struct AVCodecParameters;

class DecodeThread : public QThread
{
    Q_OBJECT
public:
    explicit DecodeThread(QObject *parent = nullptr);
    virtual ~DecodeThread();
    //将pkt加入队列
    void push(AVPacket *pkt);
    //取出一个pkt
    AVPacket *pop();
    //清理队列
    void clear();
    //关闭
    void close();
    //这是退出
    void setExit(bool isExit);
    //设置队列最大容量默认一百
    void setMaxSize(int size = 100);
    //send pkt并释放
    bool send(AVPacket* pkt);
    //recv
    AVFrame* recv();
    //找到解码器并创建配置解码器上下文
    bool codecInit(AVCodecParameters *para);

    void flushBuf();
public:
    //当前的pts
    long long pts = 0;
signals:

protected:
    //队列最大容量
    int m_maxSize = 100;
    //是否退出
    bool m_isExit = false;
    //互斥锁
    std::mutex m_mutex;
    //缓冲队列
    std::queue<AVPacket*> m_pktQue;
    //编码器
    const AVCodec *m_codec = nullptr;
    //解码器上下文
    AVCodecContext* m_codec_ctx =nullptr;
};

#endif // DECODETHREAD_H
