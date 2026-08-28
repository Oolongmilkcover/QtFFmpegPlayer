#ifndef FRAMEQUEUE_H
#define FRAMEQUEUE_H

#include <array>
#include <mutex>
extern "C"
{
#include <libavformat/avformat.h>
}

struct Frame{
    AVFrame* m_frame = nullptr;
    int m_serial = 0;
    Frame(AVFrame* frame = nullptr , int serial = 0)
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


class FrameQueue
{
private:
    // FrameQueue最多保存16帧
    static constexpr int MAX_QUEUE_SIZE = 16;

    // 环形数组
    std::array<Frame, MAX_QUEUE_SIZE> m_queue;

    // 读位置
    int m_rindex = 0;

    // 写位置
    int m_windex = 0;

    // 当前队列中的Frame数量
    int m_size = 0;

    // 实际使用的最大容量
    int m_maxSize = MAX_QUEUE_SIZE;

    // 是否保留当前显示的上一帧
    bool m_keep_last = true;

    // 0：当前帧还没有被显示
    // 1：当前帧已经被显示
    int m_rindex_shown = 0;

    // 是否中止
    bool m_abort = false;


    //上一帧
    //int prevFrame = -1;
    //逐帧用这一帧  只能往上读取一帧
    int m_curFrame = -1;
    //下一帧
    //int nextFrame = -1;
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

    // 获取下一帧
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
};

#endif // FRAMEQUEUE_H
