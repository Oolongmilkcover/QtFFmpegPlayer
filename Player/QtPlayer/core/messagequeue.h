#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H
#include<functional>
#include <mutex>

//UI->Core
enum PlayerCmd {
    CMD_OPEN        = 1,               // arg1=0, obj=std::string*(路径/URL)；成功后发 EVT_PREPARED
    CMD_PLAY,                          // 开始/恢复播放（内部按状态机判断 PREPARED→STARTED / PAUSED→STARTED）
    CMD_PAUSE,                         // STARTED→PAUSED
    CMD_SEEK,                          // arg1=目标毫秒；完成后发 EVT_SEEK_DONE
    CMD_STEP_FORWARD,                  // 暂停态逐帧前进
    CMD_STEP_BACKWARD,                 // 暂停态逐帧后退（依赖 48 帧历史）
    CMD_SET_RATE,                      // arg1=速率×100（100=1.0x，可选功能）
    CMD_STOP,                          // 停止并释放解码/渲染线程，回到 IDLE
    CMD_QUIT,                          // 退出消息循环（析构前）
};

//Core->UI
enum PlayerEvent {
    EVT_PREPARED     = 100,            // arg1=时长ms, arg2=(宽<<16)|高
    EVT_POSITION,                      // arg1=当前播放位置ms（100~200ms 一次，别每帧发）
    EVT_STATE_CHANGED,                 // arg1=新状态（IDLE/INITIALIZED/PREPARING/PREPARED/STARTED/PAUSED/COMPLETED/STOPPED/ERROR）
    EVT_DISABLEBTN,                    //关闭按钮
    EVT_ABLEBTN,                       //开启按钮
    EVT_PLAYNEXT,                      //当前视频播放完毕请求播放下一个
    EVT_SEEK_DONE,                     // arg1=实际落点ms（seek 可能不精确，要对齐关键帧）
    EVT_BUFFERING,                     // arg1=1 开始缓冲 / 0 结束（网络流才有意义）
    EVT_COMPLETED,                     // 播到文件尾
    EVT_ERROR,                         // arg1=错误码, obj=std::string*(错误文本)
};

//DemuxThread -> PlayerCore
enum ContextChange{
    CTX_IS_EXIT       = 200,            //arg1 = true/false   1/0
    CTX_IS_PAUSE,                       //arg1 = 1/0
    CTX_HAS_VIDEO,                      //arg1 = 1/0
    CTX_HAS_AUDIO,                      //arg1 = 1/0
    CTX_IS_SEEKING,                     //arg1 = 1/0
    CTX_SEEKMS,                         //arg1 = ms
    CTX_HAS_PLAYLIST,                   //arg1 = 1/0
    CTX_IS_FRAME_STEP,                  //arg1 = 1/0
    CTX_IS_STREAM_MEDIA,                //arg1 = 1/0
    CTX_SPEED,                          //arg1 = 10*speed
};


struct PlayerMessage{
    int what;       // 消息类型 (枚举)
    int arg1;       // 附加参数 1
    int arg2;       // 附加参数 2
    void* obj;      // 复杂数据指针 (比如 AVFrame*)

    // 释放 obj 的回调函数。这样队列在丢弃消息时，能自动释放对象，防止内存泄漏。
    std::function<void(void*)> free_l;

    PlayerMessage() : what(0), arg1(0), arg2(0), obj(nullptr) {}
};

struct MessageNode{
    PlayerMessage msg;
    MessageNode* next = nullptr;
};

class MessageQueue
{
public:
    MessageQueue();
    ~MessageQueue();

    //禁止拷贝
    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    void start();

    void abort();

    void clear();

    void push(PlayerMessage&& msg);

    bool pop(PlayerMessage& outMsg, bool block = true);

    PlayerMessage getMsg(int what = 0 ,int arg1 = 0 , int  arg2 = 0 , void* obj = nullptr, std::function<void(void*)> free_l = nullptr )
    {
        PlayerMessage msg;
        msg.what = what;
        msg.arg1 =arg1;
        msg.arg2 =arg2;
        msg.obj  =obj;
        msg.free_l = std::move(free_l) ;
        if(msg.obj&& !msg.free_l){
            msg.what = EVT_ERROR;
            msg.obj = new std::string("this message created failed");
            msg.free_l = [](void* p){
                delete static_cast<std::string*>(p);
            };
        }
        return msg;
    }

private:

    void destroyAllNodes();

private:
    MessageNode* m_first = nullptr;       // 队头
    MessageNode* m_last = nullptr;        // 队尾
    MessageNode* m_recycle_pool = nullptr;// 回收对象池

    int m_nb = 0;                         // 当前消息数
    int m_total_allocated = 0;            // 累计分配过的节点数
    bool m_abort = false;

    std::mutex m_mutex;
    std::condition_variable m_cond;
};

#endif // MESSAGEQUEUE_H
