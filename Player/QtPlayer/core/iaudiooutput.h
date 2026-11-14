#ifndef IAUDIOOUTPUT_H
#define IAUDIOOUTPUT_H

#include <cstdint>

/*
 * 音频出口（core 对"声卡"的唯一抽象）
 *
 * 分层约定：
 *   - 本文件属于 core，**零 Qt、零平台依赖**，core 只 include 这一个头；
 *   - platform/ 下的实现（AudioPlayer / QtAudioOutput）继承它，由外部创建后注入；
 *   - AudioThread 只借用一个指针，**不负责生命周期**，析构时不要 delete。
 *
 * 为什么是"写 + 回压"而不是 void(AVFrame*) 的回调：
 *   视频的节奏由渲染线程决定（按音频时钟睡 / 丢帧），推一帧就完事；
 *   音频的节奏由声卡决定（48kHz 硬件时钟），生产者必须知道设备还剩多少空间，
 *   否则只能无界缓冲（延迟越来越大）或静默丢帧（爆音）。
 *   所以 write() 必须返回实际写入字节，getFree() 提供剩余空间。
 *
 * PCM 格式约定：S16（short）交错排列（LRLRLR...），
 *   采样率与声道数在 open() 时给定，实现内部按这个格式配置设备。
 *   重采样（swr）与倍速（atempo）都在 core 侧完成，出口只负责把字节塞进设备。
 *
 * 线程约定：
 *   - open()/close()/clear() 只在开始、停止、seek 时调用；
 *   - write()/getFree()/getNoPlayMs() 只在**音频解码线程**调用；
 *   - setPause()/setVolume() 可能来自 UI 线程（经 PlayerCore 的消息队列）。
 *   实现内部必须自己加锁保护设备句柄（QAudioSink 的 start/suspend 不是线程安全的）。
 *
 * 命名沿用 platform/AudioPlayer 原有的方法名，目的是让迁移只改"指针类型"，
 * 不必同时改名和改调用点。
 */
class IAudioOutput
{
public:
    virtual ~IAudioOutput() = default;

    /*
     * 打开设备。sampleRate/channels 用 core 重采样后的输出参数
     * （取代原来"先给对象字段 sampleRate/channels 赋值、再无参 open()"的写法）。
     * 返回 false 表示设备不可用（被占用、格式不支持等），调用方应降级处理。
     */
    virtual bool open(int sampleRate, int channels) = 0;

    //关闭设备并释放句柄；可重复调用
    virtual void close() = 0;

    //丢掉设备缓冲里还没播放的数据（seek / 切换文件时用，避免先播出旧位置的一小段）
    virtual void clear() = 0;

    /*
     * 写入 PCM 数据，bytes 应按采样帧对齐（sampleSize/8 * channels）。
     * 返回值：
     *   > 0 : 实际写入的字节数
     *   = 0 : 设备缓冲已满（调用方应等待后重试，不要忙转）
     *   < 0 : 设备错误
     */
    virtual int write(const uint8_t *data, int bytes) = 0;

    // 设备缓冲还能接收多少字节（回压用）；设备未打开时返回 0
    virtual int getFree() = 0;

    /*
     * 设备缓冲里还没播放的时长（毫秒）——音频时钟的延迟补偿量。
     * 主时钟 = 最后写入那一帧的 PTS - getNoPlayMs() * speed
     * （暂停期间应保持不变，否则 resume 时时钟会跳）
     */
    virtual long long getNoPlayMs() = 0;

    virtual void setPause(bool isPause) = 0;
};

#endif // IAUDIOOUTPUT_H
