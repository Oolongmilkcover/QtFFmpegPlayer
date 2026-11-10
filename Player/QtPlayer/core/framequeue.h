#ifndef FRAMEQUEUE_H
#define FRAMEQUEUE_H

#include <QObject>
#include <array>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

struct Frame{
    AVFrame* m_frame = nullptr;
    int m_serial = 0;
    Frame(AVFrame* frame = nullptr , int serial = -1)
        :m_frame(frame)
        ,m_serial(serial){

    }
    ~Frame(){
        if(m_frame) av_frame_free(&m_frame);
    }

    //禁止拷贝
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    //支持移动构造
    Frame(Frame&& other) noexcept
        : m_frame(other.m_frame)
        , m_serial(other.m_serial){
        other.m_frame = nullptr;
    }

    Frame& operator=(Frame&& other) noexcept{
        if(this!=&other){
            if(m_frame) av_frame_free(&m_frame);
            m_frame =other.m_frame;
            m_serial = other.m_serial;
            other.m_frame = nullptr;
        }
        return *this;
    }

};

/*
 * FrameQueue 里有两个队列：
 *
 * 1. 未来队列 m_queue（环形数组）：解码线程写入、渲染线程消费，
 *    保存“已经解码、还没轮到显示”的帧。
 *
 * 2. 历史队列 m_history（双端队列）：已经显示（被 next() 消费）过的帧，
 *    按时间升序排列，front 最旧、back 最新，m_cursor 指向当前屏幕上的那一帧。
 *    逐帧回退就是让 m_cursor 往前走；走到头（front）就通过 onNeedRefill
 *    请求 demux 线程向后 seek 一小段并回填一批更早的帧。
 */
class FrameQueue  : public QObject
{
    Q_OBJECT
public:
    //逐帧结果
    enum StepStatus{
        StepOk = 0,      //成功
        StepEmpty,       //暂时没有帧（解码还没跟上），稍后重试
        StepNeedRefill,  //已到历史最旧处，正在回填，稍后重试
        StepAtBegin      //已经到文件开头，无法再后退
    };

    // 未来队列最大容量
    static constexpr int MAX_QUEUE_SIZE = 16;
    // 历史队列最大容量（逐帧最多能连续回退这么多帧，之后靠回填续上）
    // 注意：回填是往“更早”的方向插帧，历史满了只能牺牲离光标最远的“最新”帧，
    //       所以这个值要明显大于一次回填的帧数
    static constexpr int MAX_HISTORY_SIZE = 48;

    //帧队列本质是一个环形数组，以O(1)的时间复杂度来快速访问元素
    //keep_last = true 时会额外维护历史队列，用于逐帧回退
    FrameQueue(int max_size = MAX_QUEUE_SIZE, bool keep_last = true);

    ~FrameQueue();

    // 禁止拷贝
    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // 获取一个可以写入的Frame
    Frame* getWritable();

    // 写入完成，推进write index
    void push();

    // 获取当前可以读取的Frame
    Frame* getReadable();

    // 消费当前Frame（该帧进入历史队列）
    void next();

    // 中止等待
    void abort();

    // 恢复队列
    void reset();

    // 清空未来队列 + 历史队列
    void clear();

    // 只清未来队列（保留历史，逐帧回填 seek 时用）
    void clearFuture();

    // 当前未来队列Frame数量
    int size();

    // 当前是否中止
    bool isAborted() const;

    // ========================= 逐帧 =========================

    // 前进一帧：优先取历史里已经解码但还没显示到的帧，其次消费未来队列
    // 返回堆上的副本，调用者负责 delete；返回 nullptr 表示暂时取不到（稍后重试）
    Frame* stepForward();

    // 后退一帧：返回堆上的副本，调用者负责 delete
    // *status 说明结果：StepOk 成功 / StepEmpty 无历史 /
    //                   StepNeedRefill 已发起回填 / StepAtBegin 已到开头
    Frame* stepBackward(StepStatus* status = nullptr);

    // 回填历史：frames 为 pts 升序、且都早于当前历史最旧帧的帧
    // 内部接管 frames 里的 AVFrame 所有权，只保留最靠近边界的一批
    void prependHistory(std::deque<AVFrame*>& frames, int serial);

    // 回填结束（gotFrames=false 表示已到文件开头，之后不再请求回填）
    void finishRefill(bool gotFrames);

    // 放弃等待中的回填（退出逐帧时调用）
    void clearRefillPending();

    // 是否正在等待回填
    bool refillPending();

    // 历史最旧帧的 pts（无历史返回 -1）
    long long historyOldestPts();

    // 当前光标所在帧的 pts（-1 表示未知）
    long long currentPts();

    // 光标是否停在最新一帧（没有回退过）
    bool cursorAtNewest();

    // 历史队列当前帧数
    int historySize();

    // 历史耗尽时的回调，在调用 stepBackward 的线程里执行
    std::function<void(long long pts)> onNeedRefill;

private:
    // 把消费掉的帧放进历史（需要在持锁状态下调用，会 move 走 src 的帧数据）
    void appendHistoryLocked(Frame& src, int serial);

    // 光标是否有效
    bool cursorValidLocked() const;

private:
    //环形数组->未来队列
    std::array<Frame, MAX_QUEUE_SIZE> m_queue;

    // 读位置
    int m_rindex = 0;

    // 写位置
    int m_windex = 0;

    // 当前队列中的Frame数量
    int m_size = 0;

    // 实际使用的最大容量
    int m_maxSize = MAX_QUEUE_SIZE;

    //这个变量判断是否创建历史帧队列
    bool m_keep_last = true;

    // 是否中止
    bool m_abort = false;

    //环形数组->历史队列（front 最旧，back 最新）
    std::deque<Frame> m_history;

    // 当前显示帧在历史队列中的下标
    int m_cursor = -1;

    // 是否正在等待回填
    bool m_refillPending = false;

    // 是否已经确定退到文件开头了
    bool m_atBegin = false;

    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
};



#endif // FRAMEQUEUE_H
