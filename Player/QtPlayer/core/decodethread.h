/*
发送 AVPacket
接收 AVFrame
音频、视频都继承它

*/
#ifndef DECODETHREAD_H
#define DECODETHREAD_H

#include <atomic>
#include<mutex>
#include "core/framequeue.h"
#include "core/packetqueue.h"
// FFmpeg 的类型都是 typedef struct 出来的，前置声明必须写 struct。
// 写成 class 的话 MSVC 会把 std::function<void(AVFrame*)> 的模板参数修饰成 PEAVAVFrame（class），
// 与另一个 TU 里按 struct 生成的 PEAUAVFrame 不是同一个符号 → LNK2019 无法解析的外部符号。
struct AVCodec;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct AVCodecParameters;
/*
 * 解码器助手：包队列 / 帧队列 / 解码器初始化与收发。
 *
 * 它不再负责线程（原先是 QThread 子类）：
 *   - 视频解码线程由 VideoDecodeThread 自己持有；
 *   - 音频解码/播放线程由 AudioThread 自己持有（一直是 std::thread）。
 * 这样基类只依赖 FFmpeg + 标准库，便于后续 core 化。
 *
 */

//视频输出回调
using PaintFrame = std::function<void(AVFrame* frame)>;

class DecodeThread
{
public:
    DecodeThread();
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
