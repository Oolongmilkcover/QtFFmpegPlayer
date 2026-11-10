#ifndef MESSAGELOOPTHREAD_H
#define MESSAGELOOPTHREAD_H

#include <thread>
#include <atomic>
#include <functional>
//#include "core/messagequeue.h"

class PlayerMessage;
class MessageQueue;

class MessageLoopThread
{
public:
    // 定义消息处理回调。外部（如 PlayerCore）注册这个回调，决定怎么处理消息
    using MessageHandler = std::function<void(PlayerMessage& msg)>;

    MessageLoopThread();
    ~MessageLoopThread();

    // 禁止拷贝
    MessageLoopThread(const MessageLoopThread&) = delete;
    MessageLoopThread& operator=(const MessageLoopThread&) = delete;

    // 设置消息分发回调
    void setMessageHandler(MessageHandler handler);

    // 启动消息循环
    void start();

    void stop();

    // 对外提供的投递消息 API
    void postMessage(PlayerMessage&& msg);

    // 辅助接口：直接传参组装消息
    void postMessage(int what, int arg1 = 0, int arg2 = 0, void* obj = nullptr,std::function<void(void*)> free_l = nullptr);

private:
    //主循环
    void loop();



private:
    std::thread m_thread;
    std::atomic<bool> m_running{false};    // 运行标志
    MessageQueue* m_queue ;  // 消息队列
    MessageHandler m_handler = nullptr;    // 业务分发回调
};

#endif // MESSAGELOOPTHREAD_H
