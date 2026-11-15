#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H
#include <QObject>
#include <mutex>
#include "core/iaudiooutput.h"
class QAudioSink;
class AudioPlayer : public QObject , public IAudioOutput
{
    Q_OBJECT
public:
    int sampleRate = 48000;
    int sampleSize = 16;
    int channels = 2;

    //打开音频播放
    bool open(int sampleRate, int channels) override ;
    void close()override;
    void clear()override;
    //返回缓冲中还没有播放的时间（毫秒）
    long long getNoPlayMs()override;
    //播放音频
    int write(const unsigned char *data, int datasize) override;
    int getFree()override;
    void setPause(bool isPause)override;
    // 音量不属于 IAudioOutput 接口（core 不关心音量），由 app 侧直接调这个类
    void setVolume(qreal vol);
    double  getVolume();
    AudioPlayer();
    ~AudioPlayer();

private:
    QAudioSink *m_sink = nullptr;  // Qt6 用 QAudioSink 替代 QAudioOutput
    QIODevice *m_io = nullptr;
    std::mutex m_mutex;
    bool m_isPaused = false;
    //double
    qreal m_savedVolume = 0.5;
};

#endif // AUDIOPLAYER_H
