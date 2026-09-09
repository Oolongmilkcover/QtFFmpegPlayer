#ifndef FRAMEQUEUE_H
#define FRAMEQUEUE_H

#include <QObject>
#include <array>
#include <mutex>
extern "C"
{
#include <libavformat/avformat.h>
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

class FrameQueue  : public QObject
{
    Q_OBJECT
private:
    // FrameQueue最多保存16帧
    static constexpr int MAX_QUEUE_SIZE = 16;

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


    std::mutex m_mutex;
    std::condition_variable m_cond;

public:
    //帧队列本质是一个环形数组，以O(1)的时间复杂度来快速访问元素,最小数组大小为16，保留上一帧
    FrameQueue(int max_size = 16, bool keep_last = true);

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

    // 消费当前Frame
    void next();

    // 获取下一帧 调用者无需next
    Frame* getNextFrame();

    // 获取上一帧
    Frame* getPrevFrame();

    // 中止等待
    void abort();

    // 恢复队列
    void reset();

    // 清空队列
    void clear();

    // 当前Frame数量
    int size();

    // 当前是否中止
    bool isAborted();


signals:
    void seekToPush(int64_t pts);


private:
    // FrameQueue最多保存16帧
    static constexpr int MAX_PLAYBACKQUEUE_SIZE = 40;
    //环形数组->回放队列->模拟栈的先进后出
    std::array<Frame, MAX_PLAYBACKQUEUE_SIZE> m_playBackQueue;
    //回放帧队列大小
    int m_PBQMaxSize = MAX_PLAYBACKQUEUE_SIZE;
    //回放帧队列元素个数
    int m_PBQSize = 0;
    //栈底索引 负责判断倒退是否到底了
    int m_bottomIndex = -1;
    //栈顶索引  与m_curIndex一同判断是在历史帧内便利还是逐帧渲染新的帧
    int m_topIndex = -1;
    int m_curIndex = -1;

    std::mutex m_PBQ_mutex;

    //将这个帧移动到回放队列
    void moveToPBQ(Frame* frame);

    std::atomic<bool> m_switchNextMode = true;
    std::atomic<bool> m_PrevToNext = false;
    std::atomic<bool> m_NextToPrev = false;
};



#endif // FRAMEQUEUE_H
