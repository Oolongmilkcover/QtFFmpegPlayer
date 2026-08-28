#ifndef AUDIOTHREAD_H
#define AUDIOTHREAD_H

#include "audioplayer.h"
#include "decodethread.h"

#include <thread>
#include <atomic>
#include <mutex>

class SwrContext;

class AudioThread : public DecodeThread
{
    Q_OBJECT

public:
    explicit AudioThread(int frameQueSizeize = 100 , bool keep_last = false);
    ~AudioThread();

    // 初始化音频
    bool open(AVStream *audioStream);

    // 关闭
    void close();

    // 清空
    void clear();

    // 暂停
    void setPause(bool isPause);

    // 启动音频线程
    void startAudio();

    void sendPts(int64_t ptsMs);

    // 获取当前音频时钟
    long long getPts();

    void run();

    void setSerial(int serial);

    void setVolume(double& pos);

    double getVolume();
private:

    // 解码线程
    void decodeRun();

    // 播放线程
    void playRun();

    // 重采样初始化
    bool resampleInit();


private:

    // 音频播放器
    AudioPlayer *m_auPlayer = nullptr;

    // 重采样上下文
    SwrContext *m_swr_ctx = nullptr;

    // 重采样锁
    std::mutex m_auMutex;

    // 暂停
    std::atomic<bool> m_isPause = false;



    // 解码线程
    std::thread m_decodeThread;

    // 播放线程
    std::thread m_playThread;

    // 是否运行
    std::atomic<bool> m_running = false;

    // 当前音频时钟
    std::atomic<long long> m_audioPts = 0;

    // 输出采样率
    int m_outSampleRate = 48000;

    // 输出声道数
    int m_outChannels = 2;

    // 输出格式
    AVSampleFormat m_outSampleFmt = AV_SAMPLE_FMT_S16;

    AVStream *m_audioStream = nullptr;
};

#endif
