#include "audioplayer.h"
#include <QAudioSink>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QIODevice>
#include <mutex>
#include <QDebug>

class SingleAudioPlayer:public AudioPlayer{
private:
    QAudioSink *m_sink = nullptr;  // Qt6 用 QAudioSink 替代 QAudioOutput
    QIODevice *m_io = nullptr;
    std::mutex m_mutex;
    bool m_isPaused = false;
    //double
    qreal m_savedVolume = 0.5;

public:    
    bool open(){
        close();
        //音频格式配置
        QAudioFormat fmt;
        fmt.setSampleRate(sampleRate);
        fmt.setChannelCount(channels);
        fmt.setSampleFormat(QAudioFormat::Int16);
        if(!fmt.isValid()){
            qDebug() << "无效音频格式";
            return false;
        }

        // ==========获取默认音频设备 ==========
        const QAudioDevice &device = QMediaDevices::defaultAudioOutput();
        if(!device.isFormatSupported(fmt)){
            qDebug()<<"音频格式不支持";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        // ========== Qt6创建 QAudioSink ==========
        //音频播放核心对象
        m_sink = new QAudioSink(device, fmt, this);
        m_sink->setVolume(m_savedVolume);

        // 启动音频
        m_io = m_sink->start();
        if (!m_io) {
            qDebug() << "启动音频失败";
            delete m_sink;
            m_sink = nullptr;
            return false;
        }
        m_isPaused = false;
        return true;
    }
    
    void clear(){
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_io) {
            m_io->reset();
        }
    }
    
    void close(){
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_io) {
            m_io->close();
            m_io = nullptr;
        }
        if (m_sink) {
            m_sink->stop();
            delete m_sink;
            m_sink = nullptr;
        }
    }

    long long getNoPlayMs(){
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sink) return 0;

        long long pts = 0;
        // 还未播放的字节数
        double size = m_sink->bufferSize() - m_sink->bytesFree();
        // 一秒音频字节大小
        double secSize = sampleRate * (sampleSize / 8) * channels;
        if (secSize > 0) {
            pts = static_cast<long long>((size / secSize) * 1000);
        }
        return pts;
    }
    
    void setPause(bool isPause){
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_sink)
            return;

        // 如果当前状态已经一致，直接返回
        if (m_isPaused == isPause)
            return;

        if (isPause) {
            // 暂停
            m_sink->suspend();
            if(m_io)m_io->reset();
        } else {
            //恢复
            m_sink->resume();
        }

        m_isPaused = isPause;

    }
    
    bool write(const unsigned char *data, int datasize){
        if(!data || datasize<=0){
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        if(!m_sink||!m_io){
            return false;
        }
        int size = m_io->write((char*)data,datasize);
        return datasize==size;
    }
    
    int getFree(){
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sink) return 0;
        return m_sink->bytesFree();
    }

    void setVolume(qreal vol){
        std::lock_guard<std::mutex> lock(m_mutex);
        m_savedVolume = vol;
        if (!m_isPaused && m_sink) {  // 增加空判断
            m_sink->setVolume(vol);
        }
    }
};

AudioPlayer *AudioPlayer::getPlayer()
{
    static SingleAudioPlayer player;
    return &player;
}

AudioPlayer::AudioPlayer()
{
}


AudioPlayer::~AudioPlayer()
{
}
