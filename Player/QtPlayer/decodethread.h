/*
发送 AVPacket
接收 AVFrame
音频、视频都继承它

*/
#ifndef DECODETHREAD_H
#define DECODETHREAD_H

#include <QThread>
#include<mutex>
#include "framequeue.h"
#include"packetqueue.h"
class AVCodec;
class AVCodecContext;
class AVPacket;
class AVFrame;
class AVCodecParameters;
class DecodeThread : public QThread
{
    Q_OBJECT
public:
    explicit DecodeThread(QObject *parent = nullptr);
    virtual ~DecodeThread();
    //将pkt加入队列
    void push(AVPacket *pkt,int serial);
    //尝试将pkt加入队列，最多等 timeoutMs 毫秒
    //返回 false 表示没有入队，此时 pkt 仍然由调用者持有（不会释放）
    bool tryPush(AVPacket *pkt,int serial,int timeoutMs);
    //关闭
    void close();
    //这是退出
    void setExit(bool isExit);
    //设置队列最大容量默认一百
    void setMaxSize(int size = 100);
    //send pkt并释放
    bool send(std::unique_ptr<Packet>& pkt);
    bool send(AVPacket* pkt);
    //recv
    AVFrame* recv();
    //调用队列的clear
    void clear();
    /*
     * 只中止（唤醒）队列，不 join、不释放任何资源。
     * 用途：停机时先把可能阻塞在队列上的线程唤醒，再去 join，
     * 否则"解码线程拿着 m_decodeGate 阻塞在 getWritable()"会把
     * 同时需要闸门的线程（如 demux 线程的回填）卡到 join 永远不返回。
     */
    void abortQueues();
    //找到解码器并创建配置解码器上下文
    bool codecInit(AVCodecParameters *para);

    void flushBuf();




public:
    //当前的pts
    std::atomic<long long> pts = {0};
signals:

protected:
    //队列最大容量
    int m_maxSize = 100;
    //是否退出
    std::atomic<bool> m_isExit = false;
    //互斥锁
    std::mutex m_mutex;
    //Packet缓冲队列
    PacketQueue* m_pktQue;
    //Frame环形队列
    FrameQueue* m_frameQue;
    //编码器
    const AVCodec *m_codec = nullptr;
    //解码器上下文
    AVCodecContext* m_codec_ctx =nullptr;
    //序列serial
    std::atomic<int> m_serial = 0;

};

#endif // DECODETHREAD_H
