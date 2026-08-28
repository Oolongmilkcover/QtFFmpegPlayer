#include "packetqueue.h"

#include <QDebug>

PacketQueue::PacketQueue(int maxSize)
    : m_maxSize(maxSize > 100 ? maxSize : 100)
{
}


PacketQueue::~PacketQueue()
{
    clear();
}


// 放入Packet
bool PacketQueue::push(AVPacket* pkt, int serial)
{
    if (!pkt)
        return false;

    std::unique_lock<std::mutex> lock(m_mutex);

    // 队列满了就等待
    m_cond.wait(lock, [this]()
                {
                    return m_abort ||
                           static_cast<int>(m_queue.size()) < m_maxSize;
                });

    // 被abort
    if (m_abort)
    {
        av_packet_free(&pkt);
        return false;
    }

    // 队列接管pkt所有权
    m_queue.emplace(pkt, serial);

    // 通知消费者
    m_cond.notify_one();

    return true;
}


// 获取Packet
std::unique_ptr<Packet> PacketQueue::pop(bool block)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (block)
    {
        m_cond.wait(lock, [this]()
                    {
                        return m_abort || !m_queue.empty();
                    });
    }
    else
    {
        if (m_queue.empty())
            return nullptr;
    }

    // abort或者没有数据
    if (m_queue.empty())
        return nullptr;

    // 移动队头Packet
    auto packet =
        std::make_unique<Packet>(
            std::move(m_queue.front())
            );

    // 删除队头
    m_queue.pop();

    // 通知生产者
    m_cond.notify_one();

    return packet;
}


// 清空
void PacketQueue::clear()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        while (!m_queue.empty())
        {
            // Packet析构函数自动释放AVPacket
            m_queue.pop();
        }
    }

    // 防止有线程正在等待
    m_cond.notify_all();
}


// 中止
void PacketQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_abort = true;
    }

    // 唤醒所有阻塞线程
    m_cond.notify_all();
}


// 恢复
void PacketQueue::reset()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_abort = false;
    }

    m_cond.notify_all();
}


// 获取大小
int PacketQueue::size()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return static_cast<int>(m_queue.size());
}


// 获取abort状态
bool PacketQueue::isAborted()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_abort;
}
