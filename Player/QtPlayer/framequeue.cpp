#include "framequeue.h"
#include <algorithm>
#include <QDebug>


FrameQueue::FrameQueue(int max_size, bool keep_last)
    :m_maxSize(std::clamp(max_size, 1, MAX_QUEUE_SIZE))// 上限锁死 16
    ,m_keep_last(keep_last)
{
    // 预先分配16个AVFrame
    for(auto& frame: m_queue){
        frame.m_frame = av_frame_alloc();
    }
}

FrameQueue::~FrameQueue()
{
    for(auto& frame: m_queue){
        if(frame.m_frame){
            av_frame_free(&frame.m_frame);
        }
    }
}

Frame *FrameQueue::getWritable()
{
    // static int count1 = 1;
    // qDebug()<<"getWritable"<<count1++;
    std::unique_lock<std::mutex> lock(m_mutex);

    // 队列满了就等待
    // abort之后立即退出
    m_cond.wait(lock,[this](){
        return m_abort || m_size < m_maxSize ;
    });
    if(m_abort) return nullptr;
    return &m_queue[m_windex];
}

void FrameQueue::push()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 写入完成
        m_windex = (m_windex+1)%m_maxSize;
        ++m_size;
    }

    // 通知可能正在等待Frame的消费者
    m_cond.notify_one();
}

// 获取可读Frame
Frame *FrameQueue::getReadable()
{
    //static int count = 1;

    std::unique_lock<std::mutex> lock(m_mutex);

    m_cond.wait(lock,[this](){
        return m_abort || (m_size - m_rindex_shown > 0);
    });
    if(m_abort) return nullptr;

    int index = (m_rindex + m_rindex_shown) % m_maxSize;

    m_curFrame = index;
    return &m_queue[index];
}

// 消费当前Frame
void FrameQueue::next()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 如果要求保留上一帧
        // 第一次next不真正删除Frame
        if (m_keep_last && !m_rindex_shown)
        {
            m_rindex_shown = 1;
            return;
        }

        // 当前Frame已经不需要了
        // 这里只释放Frame内部的数据引用
        // 不释放AVFrame对象本身
        av_frame_unref(m_queue[m_rindex].m_frame);
        m_queue[m_rindex].m_serial = -1;

        // 读取位置向前移动
        m_rindex = (m_rindex + 1) % m_maxSize;

        // 队列中Frame数量减少
        if(m_size > 0){
            --m_size;
        }else{
            m_size = 0;
        }


        // 当前没有处于shown状态 //这里可能出问题
        m_rindex_shown = 0;
    }

    // 通知生产者：现在有空位置了
    m_cond.notify_one();
}

Frame *FrameQueue::getNextFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if(m_size - m_rindex_shown<=0 || m_curFrame == -1){
        return nullptr;
    }
    int index = (m_curFrame+1) % m_maxSize;
    if(m_queue[index].m_serial == -1){
        return nullptr;
    }
    m_curFrame = index;
    return &m_queue[index];
}

Frame *FrameQueue::getPrevFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 当前没有保留上一帧
    if (!m_keep_last || m_curFrame == -1 )
        return nullptr;
    int index = (m_curFrame -1  + m_maxSize) % m_maxSize;
    if(m_queue[index].m_serial == -1){
        return nullptr;
    }
    m_curFrame = index;
    return &m_queue[index];
}


void FrameQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_abort = true;
    }

    // 唤醒所有等待线程
    m_cond.notify_all();
}

void FrameQueue::reset()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_abort = false;
    }

    m_cond.notify_all();
}

void FrameQueue::clear()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (int i = 0; i < m_maxSize; ++i)
        {
            av_frame_unref(m_queue[i].m_frame);
            m_queue[i].m_serial = -1;
        }

        m_curFrame = -1;
        m_rindex = 0;
        m_windex = 0;
        m_size = 0;
        m_rindex_shown = 0;
    }

    m_cond.notify_all();
}

int FrameQueue::size()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_size;
}

bool FrameQueue::isAborted()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_abort;
}







