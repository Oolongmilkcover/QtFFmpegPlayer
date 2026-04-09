/*
解码音频
重采样
播放声音
提供全局时钟 pts（音画同步基准）
*/
#ifndef AUDIOTHREAD_H
#define AUDIOTHREAD_H
#include "audioplayer.h"
#include "decodethread.h"

class SwrContext;
class AudioThread : public DecodeThread
{
    Q_OBJECT
public:
    explicit AudioThread();
    virtual ~AudioThread();

    //打开，不管成功与否都清理
    bool open(AVCodecParameters *para, int sampleRate, int channels);

    //停止线程，清理资源
    void close();
    void Clear();

    //设置暂停
    void setPause(bool isPause);

    //子线程
    void run();


private:
    //重采样初始化
    bool resampleInit();
    //返回重采样后大小,不管成功与否都释放indata空间
    int resample(AVFrame *indata, unsigned char *data);

signals:

private:
    std::mutex m_auMutex;
    AudioPlayer *m_auPlayer = nullptr;
    //重采样上下文
    SwrContext* m_swr_ctx = nullptr;
    //是否暂停
    std::atomic<bool> m_isPause = false;
};

#endif // AUDIOTHREAD_H
