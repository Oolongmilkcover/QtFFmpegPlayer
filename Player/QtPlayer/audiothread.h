#ifndef AUDIOTHREAD_H
#define AUDIOTHREAD_H

#include "audioplayer.h"
#include "decodethread.h"

#include <thread>
#include <atomic>
#include <mutex>

class SwrContext;
struct AVFilterGraph;
struct AVFilterContext;
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

    // 倍速：只设置目标速度，实际重建在解码线程做（无锁）
    void setSpeed(double speed);          // 外部（GUI）调用，仅写原子变量
    double getSpeed() const { return m_desiredSpeed.load(); }

private:

    // 解码线程
    void decodeRun();

    // 播放线程
    void playRun();

    // 重采样初始化
    bool resampleInit();


    //倍速滤镜
    // 创建 atempo 滤镜图
    bool initAtempoFilter(double speed);
    // 释放滤镜图
    void freeAtempoFilter();

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

    // 倍速成员
    // 目标速度
    std::atomic<double> m_desiredSpeed{1.0};
    // 滤镜当前速度（解码线程读写）
    double m_currentSpeed = 1.0;
    // 滤镜图
    AVFilterGraph   *m_filterGraph   = nullptr;
    // 源滤镜（喂数据）
    AVFilterContext  *m_buffersrcCtx  = nullptr;
    // atempo 滤镜
    AVFilterContext  *m_tempoCtx      = nullptr;
    // 汇滤镜（取数据）
    AVFilterContext  *m_buffersinkCtx = nullptr;

};

#endif
