#include "core/messagequeue.h"

MessageQueue::MessageQueue()
{

}

MessageQueue::~MessageQueue()
{
    // 彻底释放所有节点（包括队列里和池子里的）
    destroyAllNodes();
}

void MessageQueue::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = false;
}

void MessageQueue::abort()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = true;
    m_cond.notify_all();
}

void MessageQueue::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    MessageNode* node  = m_first;

    while(node){
        MessageNode* next = node->next;
        if(node->msg.obj && node->msg.free_l){
            node->msg.free_l(node->msg.obj);
        }
        node->next = m_recycle_pool;
        m_recycle_pool = node;
        node = next;
    }
    m_first = nullptr;
    m_last = nullptr;
    m_nb = 0;
}


void MessageQueue::push(PlayerMessage&& msg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_abort){
        if(msg.obj&&msg.free_l){
            msg.free_l(msg.obj);
        }
        return;
    }

    MessageNode* node = m_recycle_pool;
    if(node){
        m_recycle_pool = node->next;
    }else{
        node = new MessageNode();
        m_total_allocated++;
    }

    node->msg = std::move(msg);
    node->next = nullptr;

    //尾插法
    if(m_last){
        m_last->next = node;
    }else{
        m_first = node;
    }
    m_last = node;
    m_nb++;

    m_cond.notify_all();
}

bool MessageQueue::pop(PlayerMessage& outMsg, bool block)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if(block){
        m_cond.wait(lock,[this](){
            return m_abort || m_nb > 0 ;
        });
    }

    if (m_abort) return false;
    if (m_nb== 0) return false;

    //取头节点
    MessageNode* node  = m_first;
    m_first = node->next;
    if(!m_first) m_last = nullptr;
    m_nb--;

    //将消息move给外部
    outMsg = std::move(node->msg);

    //清理节点，避免悬挂指针
    node->msg = PlayerMessage();
    node->next = nullptr;

    //把节点扔进对象池回收
    node->next = m_recycle_pool;
    m_recycle_pool = node;

    return true;

}

void MessageQueue::destroyAllNodes()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 释放活跃队列
    MessageNode* node = m_first;
    while (node) {
        MessageNode* next = node->next;
        if (node->msg.obj && node->msg.free_l) node->msg.free_l(node->msg.obj);
        delete node;
        node = next;
    }

    // 释放对象池
    node = m_recycle_pool;
    while (node) {
        MessageNode* next = node->next;
        delete node;
        node = next;
    }

    m_first = m_last = m_recycle_pool = nullptr;
    m_nb = 0;
    m_total_allocated = 0;
}


PlayerMessage getErrorMsg(std::string error)
{
    PlayerMessage msg ;
    msg.what = EVT_ERROR;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.obj = new std::string(error);
    msg.free_l = [](void* p ){
        delete static_cast<std::string*>(p);
    };
    return msg;
}
