#include "framequeue.h"
#include <algorithm>
#include <QDebug>

FrameQueue::FrameQueue(int max_size, bool keep_last)
    :m_maxSize(std::clamp(max_size, 1, MAX_QUEUE_SIZE))// 上限锁死 MAX_QUEUE_SIZE
    ,m_keep_last(keep_last)
{
    // 预先分配未来队列的 AVFrame
    for(auto& frame: m_queue){
        frame.m_frame = av_frame_alloc();
    }
    // 历史队列按需增长：Frame 析构会自动释放 AVFrame
}

FrameQueue::~FrameQueue()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for(auto& frame: m_queue){
        if(frame.m_frame){
            av_frame_free(&frame.m_frame);
        }
    }
    // 历史队列里的 Frame 由 deque 析构自动释放
    m_history.clear();
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
    std::unique_lock<std::mutex> lock(m_mutex);

    m_cond.wait(lock,[this](){
        return m_abort || (m_size  > 0);
    });
    if(m_abort) return nullptr;

    return &m_queue[m_rindex];
}

// 消费当前Frame（进入历史队列）
void FrameQueue::next()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_size <= 0) return;

        Frame& cur = m_queue[m_rindex];
        int serial = cur.m_serial;

        // 读取位置向前移动
        m_rindex = (m_rindex + 1) % m_maxSize;
        // 队列中Frame数量减少
        --m_size;

        // 已经显示过的帧进入历史队列（帧数据会被搬走）
        if(m_keep_last){
            appendHistoryLocked(cur, serial);
        }
        cur.m_serial = -1;
    }

    // 通知生产者：现在有空位置了
    m_cond.notify_one();
}

Frame *FrameQueue::stepForward()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1.历史里还有“已解码但还没显示到”的帧 → 光标前进一帧
    if(m_keep_last && cursorValidLocked() && m_cursor + 1 < (int)m_history.size()){
        ++m_cursor;
        AVFrame* copy = av_frame_clone(m_history[m_cursor].m_frame);
        if(!copy) return nullptr;
        return new Frame(copy, m_history[m_cursor].m_serial);
    }

    // 2.否则从未来队列取（非阻塞：没帧就返回空，渲染线程稍后重试）
    if(m_size <= 0) return nullptr;

    Frame& cur = m_queue[m_rindex];
    if(!cur.m_frame || !cur.m_frame->data[0]){
        // 空槽位，丢弃
        m_rindex = (m_rindex + 1) % m_maxSize;
        --m_size;
        cur.m_serial = -1;
        m_cond.notify_one();
        return nullptr;
    }

    AVFrame* copy = av_frame_clone(cur.m_frame);
    int serial = cur.m_serial;

    m_rindex = (m_rindex + 1) % m_maxSize;
    --m_size;
    if(m_keep_last){
        appendHistoryLocked(cur, serial);
    }
    cur.m_serial = -1;
    m_cond.notify_one();

    if(!copy) return nullptr;
    return new Frame(copy, serial);
}

Frame *FrameQueue::stepBackward(StepStatus* status)
{
    Frame* ret = nullptr;
    long long oldestPts = -1;
    bool needRefill = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if(status) *status = StepEmpty;

        // 没有历史可退（还没显示过帧 / 非逐帧队列）
        if(!m_keep_last || !cursorValidLocked()){
            if(status) *status = StepAtBegin;
            return nullptr;
        }

        if(m_cursor == 0){
            // 已经在历史最旧帧 → 需要 demux 向后 seek 回填
            if(m_refillPending){
                if(status) *status = StepNeedRefill;
                return nullptr;
            }
            oldestPts = m_history.front().m_frame
                            ? m_history.front().m_frame->pts : -1;
            if(m_atBegin || oldestPts <= 0){
                // 已经是文件第一帧，退无可退
                if(status) *status = StepAtBegin;
                return nullptr;
            }
            m_refillPending = true;
            needRefill = true;
            if(status) *status = StepNeedRefill;
        }else{
            --m_cursor;
            AVFrame* copy = av_frame_clone(m_history[m_cursor].m_frame);
            if(copy) ret = new Frame(copy, m_history[m_cursor].m_serial);
            if(status) *status = StepOk;
        }
    }

    // 回调放在锁外，避免和回填线程相互等待
    if(needRefill && onNeedRefill){
        onNeedRefill(oldestPts);
    }

    return ret;
}

void FrameQueue::prependHistory(std::deque<AVFrame*>& frames, int serial)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if(!m_keep_last){
        for(AVFrame*& f : frames){
            if(f) av_frame_free(&f);
        }
        frames.clear();
        return;
    }

    //极端保护：一次回填就超过整个容量时，只留最靠近边界的那些帧
    while((int)frames.size() > MAX_HISTORY_SIZE){
        av_frame_free(&frames.front());
        frames.pop_front();
    }

    //容量有限：为了让“更早的帧”进来，只能丢掉离光标最远的“最新”帧。
    //（丢最旧的入帧会让回填原地打转，所以优先牺牲后面那段）
    while((int)m_history.size() + (int)frames.size() > MAX_HISTORY_SIZE
           && m_history.size() > 1
           && m_cursor < (int)m_history.size() - 1){
        m_history.pop_back();
    }
    //还是放不下（光标就停在最新帧上）才丢入帧里最旧的
    while((int)m_history.size() + (int)frames.size() > MAX_HISTORY_SIZE
           && !frames.empty()){
        av_frame_free(&frames.front());
        frames.pop_front();
    }

    // frames 是 pts 升序，倒着 push_front 之后历史里就是 旧->新 的顺序
    for(auto it = frames.rbegin(); it != frames.rend(); ++it){
        AVFrame* f = *it;
        if(!f) continue;
        m_history.push_front(Frame(f, serial));
        *it = nullptr;
        // 已有元素整体后移一格，光标跟着走才能继续指向原来那一帧
        if(m_cursor >= 0) ++m_cursor;
    }
    frames.clear();

    m_refillPending = false;
    m_atBegin = false;
}

void FrameQueue::finishRefill(bool gotFrames)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_refillPending = false;
    if(!gotFrames){
        // 回填不到更早的帧 → 已经到文件开头
        m_atBegin = true;
    }
}

void FrameQueue::clearRefillPending()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_refillPending = false;
}

bool FrameQueue::refillPending()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_refillPending;
}

long long FrameQueue::historyOldestPts()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!m_keep_last || m_history.empty() || !m_history.front().m_frame){
        return -1;
    }
    return m_history.front().m_frame->pts;
}

long long FrameQueue::currentPts()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!cursorValidLocked()) return -1;
    return m_history[m_cursor].m_frame->pts;
}

bool FrameQueue::cursorAtNewest()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!m_keep_last || m_history.empty() || m_cursor < 0) return true;
    return m_cursor + 1 >= (int)m_history.size();
}

int FrameQueue::historySize()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_history.size();
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

void FrameQueue::clearFuture()
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
}

void FrameQueue::clear()
{
    clearFuture();

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_history.clear();
        m_cursor = -1;
        m_refillPending = false;
        m_atBegin = false;
    }
}

int FrameQueue::size()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_size;
}

bool FrameQueue::isAborted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_abort;
}

bool FrameQueue::cursorValidLocked() const
{
    return m_cursor >= 0 && m_cursor < (int)m_history.size()
           && m_history[m_cursor].m_frame != nullptr;
}

void FrameQueue::appendHistoryLocked(Frame& src, int serial)
{
    if(!m_keep_last || !src.m_frame || !src.m_frame->data[0]) return;

    // clone 只是增加底层 buffer 的引用计数，不会有数据拷贝
    AVFrame* copy = av_frame_clone(src.m_frame);
    if(!copy) return;

    // 未来队列的槽位交还出去，生产者可以立刻复用
    av_frame_unref(src.m_frame);

    m_history.push_back(Frame(copy, serial));

    // 超容量就丢最旧的
    while((int)m_history.size() > MAX_HISTORY_SIZE){
        m_history.pop_front();
        if(m_cursor >= 0) --m_cursor;
    }

    // 新帧就是当前显示的帧
    m_cursor = (int)m_history.size() - 1;
    // 历史边界往后挪了，“已经退到开头”的判断作废（真正到开头由 pts<=0 兜底）
    m_atBegin = false;
}
