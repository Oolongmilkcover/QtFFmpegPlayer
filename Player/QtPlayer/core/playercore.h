#ifndef PLAYERCORE_H
#define PLAYERCORE_H
#include<functional>
#include"core/demuxthread.h"
class PlayerContext;
class MessageLoopThread;
struct PlayerMessage;
struct AVFrame;
class PlayerCore
{
public:
    PlayerCore();
    ~PlayerCore();
    using EventCallback = std::function<void(PlayerMessage& msg)>;

    //Core到UI的事件回调
    void setEventCallBack(EventCallback callBack);
    //共享上下文
    void setPlayerContext(PlayerContext* ctx);

    //多态传递
    void postMessage(PlayerMessage&& msg);
    void postMessage(int what = 0 ,int arg1 = 0,int arg2 = 0,void* obj = nullptr,std::function<void(void*)> free_l = nullptr);


    //音视频输出管道
    using PlayCallBack = std::function<void(AVFrame* frame)>;
    void setAudioPlayCallBack(PlayCallBack cb);
    void setVideoPlayCallBack(PlayCallBack cb);
    void start();
private:

    void setMessageHandler();


private:
    // 指针一律给默认值：这几个成员在构造函数里用，写漏一个就是野指针
    MessageQueue* m_msgQue = nullptr;

    MessageLoopThread* m_loop = nullptr;

    EventCallback m_callBack;

    PlayerContext* m_playerctx = nullptr;

    DemuxThread dt;

};

#endif // PLAYERCORE_H
