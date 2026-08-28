#ifndef PACKETQUEUE_H
#define PACKETQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

extern "C"
{
#include <libavcodec/packet.h>
}


// 一个Packet = AVPacket + serial
struct Packet
{
    AVPacket* m_pkt = nullptr;
    int m_serial = 0;

    Packet(AVPacket* pkt = nullptr, int serial = 0)
        : m_pkt(pkt)
        , m_serial(serial)
    {
    }

    ~Packet()
    {
        if (m_pkt)
            av_packet_free(&m_pkt);
    }

    // 禁止拷贝
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;

    // 移动构造
    Packet(Packet&& other) noexcept
        : m_pkt(other.m_pkt)
        , m_serial(other.m_serial)
    {
        other.m_pkt = nullptr;
    }

    // 移动赋值
    Packet& operator=(Packet&& other) noexcept
    {
        if (this != &other)
        {
            if (m_pkt)
                av_packet_free(&m_pkt);

            m_pkt = other.m_pkt;
            m_serial = other.m_serial;

            other.m_pkt = nullptr;
        }

        return *this;
    }
};


class PacketQueue
{
private:

    std::queue<Packet> m_queue;

    std::mutex m_mutex;

    std::condition_variable m_cond;

    // 最大Packet数量
    int m_maxSize = 100;

    // 是否中止
    bool m_abort = false;

public:

    explicit PacketQueue(int maxSize = 100);

    ~PacketQueue();

    // 禁止拷贝
    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // 放入Packet
    // Queue满时阻塞
    bool push(AVPacket* pkt, int serial);

    // 获取Packet
    // block=true：没有Packet就等待
    // block=false：没有Packet立即返回
    std::unique_ptr<Packet> pop(bool block = true);

    // 清空
    void clear();

    // 中止
    void abort();

    // 恢复
    void reset();

    // 当前大小
    int size();

    // 是否中止
    bool isAborted();
};

#endif // PACKETQUEUE_H
