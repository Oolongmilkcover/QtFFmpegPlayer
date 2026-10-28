#ifndef PUSHER_DROPQUEUE_H
#define PUSHER_DROPQUEUE_H

/*
 * ====================== 有界队列（两种"满了怎么办"的策略） ======================
 *
 * 为什么采集/推流这条链上必须有它：
 *   采集是实时源：摄像头到点就出帧，你处理不过来它也不会等你。所以
 *     - 帧队列（采集 → 编码）：满了只能丢最旧，绝不能阻塞采集；
 *   而编码线程是"消费者"，它可以等（等一下不会丢数据），所以
 *     - 包队列（编码 → 推流）：落盘时阻塞等待（等一等无所谓，文件必须完整）；
 *       推流时改成丢包（网络卡住时宁可丢，也不能让延迟越堆越大）。
 *
 * 所以这个队列把策略做成开关，两种场景共用一个实现。
 *
 * 关闭（close）的语义 —— 这里很关键，决定了"优雅退出"能不能写对：
 *   - close() 之后：push() 直接拒绝（并释放元素）；
 *   - close() 之后：pop() 仍然会把队列里剩下的元素取完，取空之后才返回 false。
 *   这样收尾顺序才能做到"先停生产、再把存货消费干净、最后写 trailer"。
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
}

// 队列持有元素的所有权：被丢弃 / 被 close 拒绝 / 析构时，都要用 Deleter 释放
struct AvFrameDeleter
{
    void operator()(AVFrame* f) const { if (f) av_frame_free(&f); }
};

struct AvPacketDeleter
{
    void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};

template <typename T, typename Deleter>
class DropQueue
{
public:
    DropQueue(size_t capacity, Deleter deleter, bool dropOldestWhenFull)
        : m_capacity(capacity ? capacity : 1)
        , m_deleter(std::move(deleter))
        , m_dropOldest(dropOldestWhenFull)
    {
    }

    ~DropQueue()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_items.empty()) {
            m_deleter(m_items.front());
            m_items.pop_front();
        }
    }

    DropQueue(const DropQueue&) = delete;
    DropQueue& operator=(const DropQueue&) = delete;

    // 放一个元素进去（所有权交给队列）。
    // 满的时候：dropOldest=true → 丢掉最旧的，收下这个新的；false → 阻塞等到有位置。
    void push(T item)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_closed) {                 // 已经收工了：拒绝并释放，绝不让元素泄漏
            lock.unlock();
            m_deleter(item);
            return;
        }
        if (m_items.size() >= m_capacity) {
            if (m_dropOldest) {
                // 丢最旧：队列里留的永远是"最近"的数据，实时链路要的就是这个
                m_deleter(m_items.front());
                m_items.pop_front();
                ++m_dropped;
            } else {
                // 背压：等消费者腾出位置（落盘场景，文件完整性优先）
                m_cv.wait(lock, [this] { return m_closed || m_items.size() < m_capacity; });
                if (m_closed) {
                    lock.unlock();
                    m_deleter(item);
                    return;
                }
            }
        }
        m_items.push_back(item);
        lock.unlock();
        m_cv.notify_one();
    }

    // 取一个元素出来（所有权交回调用方）。
    // 超时返回 false —— 调用方用它来周期性检查"是不是该退出了"，避免死等。
    bool pop(T& out, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return !m_items.empty() || m_closed; })) {
            return false;               // 超时
        }
        if (m_items.empty()) return false;   // 被 close 且已经取空
        out = m_items.front();
        m_items.pop_front();
        lock.unlock();
        m_cv.notify_all();              // 唤醒可能在等位置的阻塞 push
        return true;
    }

    // 停止生产：之后 push 被拒；但 pop 还能把存货取完
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        m_cv.notify_all();
    }

    uint64_t dropped() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_dropped;
    }

    // 生产者是否已经收工。消费者用它判断"取空了就可以退出了"——
    // 注意不能拿 m_running 之类的外部标志判断：生产者的尾巴数据可能还没写完。
    bool closed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size();
    }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::deque<T>           m_items;
    size_t                  m_capacity;
    Deleter                 m_deleter;
    bool                    m_dropOldest;
    bool                    m_closed = false;
    uint64_t                m_dropped = 0;
};

#endif // PUSHER_DROPQUEUE_H
