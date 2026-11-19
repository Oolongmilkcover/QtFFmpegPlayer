#ifndef PLAYERCORE_H
#define PLAYERCORE_H
#include<functional>
#include"core/demuxthread.h"
class PlayerContext;
class MessageLoopThread;
struct PlayerMessage;
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


    /*
     * 说明：这里原来还有一组
     *     using PlayCallBack = std::function<void(AVFrame* frame)>;
     *     void setAudioPlayCallBack(PlayCallBack cb);
     *     void setVideoPlayCallBack(PlayCallBack cb);
     * 只有声明、没有定义 —— 谁调谁链接失败。真正的音视频出口在
     * DemuxThread::openFile(name, cb, out) 里一次性注入（视频帧回调 + IAudioOutput），
     * 所以那三个是历史遗留，已删除。
     * SDK 化时会在新的 C 接口里重新设计出口（见 include/op/op_player.h 草案）。
     */

    //启动 / 同步停止（stop 会 join 消息循环和所有解码线程，返回后不会再有任何回调）
    void start();
    void stop();

private:

    void setMessageHandler();


private:
    bool m_stopped = false;
    // 指针一律给默认值：这几个成员在构造函数里用，写漏一个就是野指针
    MessageQueue* m_msgQue = nullptr;

    MessageLoopThread* m_loop = nullptr;

    EventCallback m_callBack;

    PlayerContext* m_playerctx = nullptr;

    DemuxThread dt;

};

#endif // PLAYERCORE_H
