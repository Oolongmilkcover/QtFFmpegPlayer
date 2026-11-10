#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H
#include<atomic>
#include<functional>
#include <mutex>

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
