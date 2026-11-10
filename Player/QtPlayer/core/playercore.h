#ifndef PLAYERCORE_H
#define PLAYERCORE_H

class MessageLoopThread;

class PlayerCore
{
public:
    PlayerCore();


private:
    MessageLoopThread* m_loop;

};

#endif // PLAYERCORE_H
