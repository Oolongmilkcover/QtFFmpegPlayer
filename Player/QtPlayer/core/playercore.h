#ifndef PLAYERCORE_H
#define PLAYERCORE_H
class PlayerContext;
class MessageLoopThread;

class PlayerCore
{
public:
    PlayerCore();


private:
    void setMessageHandler();

private:
    MessageLoopThread* m_loop;


    PlayerContext* m_playerctx;

};

#endif // PLAYERCORE_H
