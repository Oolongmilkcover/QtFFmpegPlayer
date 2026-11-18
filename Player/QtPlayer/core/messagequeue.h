#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H
#include <condition_variable>
#include<functional>
#include <mutex>
#include <string>   // getMsg()/getErrorMsg() 里用到 std::string；libstdc++ 的 <condition_variable> 只带前置声明，
                    // 不写这行在 MinGW/GCC 上会报 "invalid use of incomplete type 'std::string'"（MSVC 头文件会顺带包含 <string>，所以只有它编得过）

//UI->Core
enum PlayerCmd {
    CMD_OPEN        = 1,               // arg1=0, obj=std::string*(路径/URL)+PaintFrame回调函数+AudioPlyer指针；成功后发 EVT_PREPARED
    CMD_PLAY,                          // 开始/恢复播放（内部按状态机判断 PREPARED→STARTED / PAUSED→STARTED）
    CMD_PAUSE,                         // STARTED→PAUSED
    CMD_SEEK,                          // 用arg1和arg2拆分int64为高位和低位 arg1=目标毫秒；完成后发 EVT_SEEK_DONE
    CMD_SET_VOLUME,                    // 设置音量 arg1 = pos*100
    CMD_SET_SPEED,                      // arg1=速率×10（10=1.0x）
    CMD_STEP_FORWARD,                  // 暂停态逐帧前进
    CMD_STEP_BACKWARD,                 // 暂停态逐帧后退（依赖 48 帧历史）
    CMD_STEP_STOP,                      // 结束逐帧状态
    CMD_STOP,                          // 停止并释放解码/渲染线程，回到 IDLE
    CMD_QUIT,                          // 退出消息循环（析构前）

};

//Core->UI
enum PlayerEvent {
    EVT_PREPARED     = 100,            // arg1=时长ms, arg2=(宽<<16)|高
    EVT_POSITION,                      // arg1=当前播放位置ms（100~200ms 一次，别每帧发）
    EVT_STATE_CHANGED,                 // arg1=新状态（IDLE/INITIALIZED/PREPARING/PREPARED/STARTED/PAUSED/COMPLETED/STOPPED/ERROR）
    EVT_CLEARSCREEN,                   // 清屏
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
    CTX_IS_AUDIO_MEDIA,                 //arg1 = 1/0
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

/*
 * CMD_OPEN 的负载：一次打开动作需要的三样东西。
 *
 * 之前它定义在 app/player.cpp 里（只有 UI 侧看得见），
 * 而 PlayerCore 处理 CMD_OPEN 时又按 std::string* 去解引用 obj —— 类型对不上。
 * 放到 core 里，收发两端都能看到同一个定义。
 *
 * cb 是"视频帧渲染回调"（std::function<void(AVFrame*)>），
 * out 是音频出口指针，生命周期都由 app 侧（Player）持有，这里只借指针。
 */
struct AVFrame;
class IAudioOutput;

struct CMD_OPEN_OBJ
{
    std::string name;                       // 路径 / URL
    std::function<void(AVFrame*)> cb;       // 视频帧回调
    IAudioOutput *out = nullptr;            // 音频输出（可为空 → 无声音）

    CMD_OPEN_OBJ() = default;
    CMD_OPEN_OBJ(std::string n, std::function<void(AVFrame*)> c, IAudioOutput *o)
        : name(std::move(n)), cb(std::move(c)), out(o) {}
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
        if(msg.obj && !msg.free_l){
            /*
             * 走到这里说明调用方给了 obj 却没给 free_l —— 队列只知道有个指针，
             * 不知道它是什么类型、是不是 new 出来的，所以这里**不能**释放它：
             * `delete (void*)` 是未定义行为（MSVC 报 C4150，只会调 operator delete、
             * 不调析构函数），对非 new 分配或非对象类型更是直接踩内存。
             * 所以这里只把消息改写成错误消息让上层看得见，原 obj 原样留着
             * （泄漏总比崩掉好，而且这条分支本身就应该由调用方修：补 free_l）。
             */
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

PlayerMessage getErrorMsg(std::string error);

#endif // MESSAGEQUEUE_H
