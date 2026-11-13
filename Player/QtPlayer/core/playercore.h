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

private:


    void setMessageHandler();


private:
    MessageLoopThread* m_loop;

    EventCallback m_callBack;

    PlayerContext* m_playerctx;

    DemuxThread dt;
};

#endif // PLAYERCORE_H
