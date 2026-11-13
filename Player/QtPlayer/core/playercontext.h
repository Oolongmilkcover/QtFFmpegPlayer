#ifndef PLAYERCONTEXT_H
#define PLAYERCONTEXT_H
#include<atomic>
#include <string>
class  PlayerContext
{
private:
    //专属的写接口
    //只允许playCore写
    friend class PlayerCore;
    void setIsExit(bool val)              { m_isExit.store(val);}
    void setIsPause(bool val)             { m_isPause.store(val);}
    void setHasVideo(bool val)          { m_hasVideo.store(val);}
    void setHasAudio(bool val)          { m_hasAudio.store(val);}
    void setIsSeeking(bool val)         { m_isSeeking.store(val);}
    void setSeekMs(long long val)       { m_seekMs.store(val);}
    void setHasPlayList(bool val)       { m_hasPlayList.store(val);}
    void setIsFrameStep(bool val)       { m_isFrameStep.store(val);}
    void setIsStreamMedia(bool val)     { m_isStreamMedia.store(val);}
    void setSpeed(double val)           { m_speed.store(val);}
    void setSetPlayingName(std::string& name){m_playingName = name;};

public:
    //供demudex读
    bool getIsExit()          const   {return m_isExit.load();}
    bool getIsPause()         const   {return m_isPause.load();}
    bool getHasVideo()      const   {return m_hasVideo.load();}
    bool getHasAudio()      const   {return m_hasAudio.load();}
    bool getIsSeeking()     const   {return m_isSeeking.load();}
    long long  getSeekMs()  const   {return m_seekMs.load();}
    bool getHasPlayList()   const   {return m_hasPlayList.load();}
    bool getIsFrameStep()   const   {return m_isFrameStep.load();}
    bool getIsStreamMedia() const   {return m_isStreamMedia.load();}
    double getSpeed()       const   {return m_speed.load();}
    std::string getPlayingName() const{return m_playingName;};

private:
    //是否退出
    std::atomic<bool>  m_isExit = false;

    //是否暂停
    std::atomic<bool> m_isPause = false;


    //判断流是否存在
    std::atomic<bool> m_hasVideo = false;
    std::atomic<bool> m_hasAudio = false;

    //异步seek
    std::atomic<bool> m_isSeeking = false;
    //seek目标（毫秒）
    std::atomic<long long> m_seekMs = 0;

    //主程序是否有播放列表
    std::atomic<bool> m_hasPlayList = false;

    //是否在逐帧
    std::atomic<bool> m_isFrameStep = false;

    /*
     * 是否网络流媒体（openFile 成功后写入，close 复位）。
     * 流媒体的闸门：requestSeekMs / startFrameStep / stepNext|PrevFrame /
     * setSpeed 都会先看这个标志，另见 isStreamMedia()。
     */
    std::atomic<bool> m_isStreamMedia{false};

    //倍速
    std::atomic<double> m_speed{1.0};

    //正在播放的名字
    std::string m_playingName;

public:
    PlayerContext();
    ~PlayerContext();
};

#endif // PLAYERCONTEXT_H
