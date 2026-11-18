#include "core/messageloopthread.h"
#include "core/messagequeue.h"
#include "core/log.h"
MessageLoopThread::MessageLoopThread():m_running(false)  {}

MessageLoopThread::~MessageLoopThread()
{
    // 声明了很久但没有定义过；PlayerCore 会 delete 它，所以必须在这里收尾。
    // stop() 内部有 join，线程还没停就会等它退出。
    if (m_running.load() && m_queue) stop();
}

void MessageLoopThread::setMessageHandler(MessageHandler handler)
{
    m_handler = std::move(handler);
}

void MessageLoopThread::start()
{
    //先判队列再置运行标志：反过来的话"队列为空"早退会把 m_running 永久留在 true，
    //之后 stop() 会拿着 m_queue==nullptr 去 abort()（空指针解引用）。
    if (!m_queue){
        logDebug()<<"m_queue is NULL";
        return;
    }
    if (m_running.exchange(true)) return;  //只启动一次
    m_queue->start();
    m_thread = std::thread(&MessageLoopThread::loop, this);
}

void MessageLoopThread::stop()
{
    // 标志位设为 false；已经是 false 说明没在跑（或已经被停过），直接返回
    if (!m_running.exchange(false)) return;
    if (!m_queue) return;
    // 唤醒可能正在阻塞等待的队列
    m_queue->abort();
    //  等待线程真正退出
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_queue->clear();
}

void MessageLoopThread::postMessage(PlayerMessage &&msg)
{
    if (!m_queue){
        logDebug()<<"m_queue is NULL";
        return;
    }
    if (m_running.load()) {
        m_queue->push(std::move(msg));
    } else {
        // 如果队列没运行，直接释放携带的资源，防止内存泄漏
        if (msg.obj && msg.free_l) {
            msg.free_l(msg.obj);
        }
    }
}

void MessageLoopThread::postMessage(int what, int arg1, int arg2, void *obj, std::function<void (void *)> free_l)
{
    if (!m_queue){
        logDebug()<<"m_queue is NULL";
        return;
    }
    PlayerMessage msg;
    msg.what = what;
    msg.arg1 = arg1;
    msg.arg2 = arg2;
    msg.obj = obj;
    msg.free_l = std::move(free_l);
    postMessage(std::move(msg));
}

void MessageLoopThread::setMessageQueue(MessageQueue *q)
{
    if(!q || m_queue){
        return;
    }
    m_queue = q;
}

void MessageLoopThread::loop()
{
    if (!m_queue){
        logDebug()<<"m_queue is NULL";
        return;
    }
    printf("[MessageLoop] Thread started.\n");

    while (m_running.load()) {
        PlayerMessage msg;

        // 阻塞等待消息。如果队列 abort，返回 false
        if (!m_queue->pop(msg, true)) {
            break;
        }

        // 分发消息给注册的回调（比如 PlayerCore 的 switch-case）
        if (m_handler) {
            m_handler(msg);
        }

        // 核心：资源清理
        // 在这里统一释放，防止内存泄漏。
        if (msg.obj && msg.free_l) {
            msg.free_l(msg.obj);
        }
    }

    printf("[MessageLoop] Thread ended.\n");
}
