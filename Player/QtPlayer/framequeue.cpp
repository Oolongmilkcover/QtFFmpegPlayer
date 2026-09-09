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

    if(m_keep_last){
        // 预先分配40个AVFrame给回放数组
        for(auto& frame: m_playBackQueue){
            frame.m_frame = av_frame_alloc();
        }
    }
}

FrameQueue::~FrameQueue()
{
    for(auto& frame: m_queue){
        if(frame.m_frame){
            av_frame_free(&frame.m_frame);
        }
    }
    if(m_keep_last){
        for(auto& frame: m_playBackQueue){
            av_frame_free(&frame.m_frame);
        }
    }
}

Frame *FrameQueue::getWritable()
{
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
        return m_abort || (m_size  > 0);
    });
    if(m_abort) return nullptr;

    int index = m_rindex;

    return &m_queue[index];
}

// 消费当前Frame
void FrameQueue::next()
{
    {

        // 当前Frame已经不需要了 给到回放队列内
        int index = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            index = m_rindex;
        }
        moveToPBQ(&m_queue[index]);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue[m_rindex].m_serial = -1;

        // 读取位置向前移动
        m_rindex = (m_rindex + 1) % m_maxSize;

        // 队列中Frame数量减少
        if(m_size > 0){
            --m_size;
        }else{
            m_size = 0;
        }
    }

    // 通知生产者：现在有空位置了
    m_cond.notify_one();
}

Frame *FrameQueue::getNextFrame()
{
    //如果是在回放队列内遍历就返回当前索引的frame并往前走
    //如果不在就返回未来队列的rindex,在每次getreadable后用户都会调用next，
    //那么m_rindex相比于现在屏幕上的帧就是下一帧
    if(m_switchNextMode.load()){
        m_NextToPrev.store(false);
        AVFrame *AVframe = av_frame_clone(m_queue[m_rindex].m_frame);
        Frame *frame = new Frame(AVframe,-1);
        next();
        return frame;
    }else{
        //从回放取
        std::lock_guard<std::mutex> lock(m_PBQ_mutex);
        if (m_curIndex < 0 || m_PBQSize == 0) return nullptr;

        if(m_PrevToNext){
            m_curIndex = (m_curIndex+2)%m_PBQMaxSize;
            m_PrevToNext.store(false);
        }
        int index = m_curIndex;
        if(m_curIndex == m_topIndex){
            //意味着逐下一帧要从未来取了
            m_switchNextMode.store(true);
            //m_curIndex可以不做处理，在next的时候会赋值m_curIndex = m_topIndex
        }else{
            m_curIndex = (m_curIndex+1)%m_PBQMaxSize;
        }
        m_NextToPrev.store(true);
        return &m_playBackQueue[index];
    }
    return nullptr;
}


Frame *FrameQueue::getPrevFrame()
{
    //取上一帧更加复杂，如果读到了栈底还想再读上一帧就得dumux线程seek到栈底帧pts的前一个关键帧再预解析保存到回放队列
    //如果没有到栈底就正常返回m_curIndex--
    std::lock_guard<std::mutex> lock(m_PBQ_mutex);
    // 队列为空，没有上一帧
    if (m_curIndex < 0 || m_PBQSize == 0) {
        return nullptr;
    }
    //这里是一个bug的补丁，如果上一次是取了未来的帧m_switchNextMode就是true
    //这时候m_curIndex == top 且是已经在屏幕上的帧这时候要index = m_curIndex-1
    if(m_switchNextMode.load()){
        m_curIndex = (m_curIndex - 1 + m_PBQMaxSize) % m_PBQMaxSize;
    }else if(m_NextToPrev){
        m_curIndex = (m_curIndex - 2 + m_PBQMaxSize )%m_PBQMaxSize;
        m_NextToPrev.store(false);
    }


    //如果这帧是栈底帧，返回前记录pts并发出seek信号
    int index = m_curIndex ;
    if(m_curIndex == m_bottomIndex && m_bottomIndex != -1){
        emit seekToPush(m_playBackQueue[index].m_frame->pts);
        return &m_playBackQueue[index];
    }


    m_curIndex = (index - 1 + m_PBQMaxSize) % m_PBQMaxSize;
    m_switchNextMode.store(false);
    m_PrevToNext.store(true);
    return &m_playBackQueue[index];
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
        m_rindex = 0;
        m_windex = 0;
        m_size = 0;
    }
    m_cond.notify_all();

    if(!m_keep_last) return;

    {
        std::lock_guard<std::mutex> lock(m_PBQ_mutex);

        for (int i = 0; i < m_PBQMaxSize; ++i)
        {
            av_frame_unref(m_playBackQueue[i].m_frame);
            m_playBackQueue[i].m_serial = -1;
        }
        m_PBQSize = 0;
        m_bottomIndex = -1;
        m_topIndex = -1;
        m_curIndex = -1;
        m_switchNextMode.store(true);
    }
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


void FrameQueue::moveToPBQ(Frame *frame)
{
    if(!m_keep_last || frame == nullptr || frame->m_frame==nullptr) return;
    // 若PBQ满了 释放栈底frame 并将栈底前进一格

    std::lock_guard<std::mutex> lock(m_PBQ_mutex);
    if(m_bottomIndex == -1 ) m_bottomIndex = 0;
    int serial = m_queue[m_rindex].m_serial;
    if(m_PBQSize >= m_PBQMaxSize){
        av_frame_unref(m_playBackQueue[m_bottomIndex].m_frame);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue[m_rindex].m_serial = -1;
        }

        m_bottomIndex = (m_bottomIndex+1) % m_PBQMaxSize;
    }
    m_topIndex = (m_topIndex+1)% m_PBQMaxSize;
    m_curIndex = m_topIndex;
    av_frame_move_ref(m_playBackQueue[m_topIndex].m_frame ,frame->m_frame);
    m_playBackQueue[m_topIndex].m_serial = serial;
    if(m_PBQSize<m_PBQMaxSize){
        m_PBQSize++;
    }
}

