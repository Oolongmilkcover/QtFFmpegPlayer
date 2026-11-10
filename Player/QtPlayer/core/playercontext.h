#ifndef PLAYERCONTEXT_H
#define PLAYERCONTEXT_H

#include<atomic>
class  PlayerContext
{
private:
    //专属的写接口
    friend class PlayerCore;
    void setAbort(bool val) { m_abort.store(val); }
    void setPaused(bool val) { m_isPaused.store(val); }

private:
    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_isPaused{false};
    //未完...


public:
    PlayerContext();
};

#endif // PLAYERCONTEXT_H
