#include "platform/audioplayer.h"
#include <QAudioSink>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QIODevice>
#include <mutex>
#include <QDebug>


bool AudioPlayer::open(int sampleRate, int channels){
    close();
    // 把这次真正打开的格式记进成员：getNoPlayMs() 要用它算 bytesPerSecond，
    // 否则设备实际跑在别的采样率时，音频时钟（= PTS - 未播毫秒）会按 48k 算错。
    this->sampleRate = sampleRate;
    this->channels   = channels;
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
    /*
     * 这里刻意不传 parent。
     *
     * AudioPlayer 对象是在 GUI 线程 new 出来的，而 open() 由 core 的消息循环线程
     * 经 DemuxThread → AudioThread 调进来；给一个"属于别的线程"的 QObject 当 parent
     * 会触发 Qt 的跨线程父子关系告警（Debug 下可能直接 abort）。
     * sink 的生命周期全部由 close() 里的 delete 管理，本来也不需要 parent。
     */
    m_sink = new QAudioSink(device, fmt);
    m_sink->setVolume(m_savedVolume);
    //m_sink->setBufferSize(131072*10);
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

void AudioPlayer::clear(){
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_io) {
        m_io->reset();
    }
}

void AudioPlayer::close(){
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

long long AudioPlayer::getNoPlayMs(){
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sink)
        return 0;

    const qint64 bytes =
        m_sink->bufferSize() -
        m_sink->bytesFree();

    const qint64 bytesPerSecond =
        static_cast<qint64>(sampleRate) *
        (sampleSize / 8) *
        channels;

    if (bytesPerSecond <= 0)
        return 0;

    return bytes * 1000 / bytesPerSecond;
}

void AudioPlayer::setPause(bool isPause){
    std::lock_guard<std::mutex> lock(m_mutex);
    // 如果当前状态已经一致，直接返回
    if (!m_sink||m_isPaused == isPause)
        return;
    if (isPause) {
        // 暂停
        m_sink->suspend();
    } else {
        //恢复
        m_sink->resume();
        m_sink->setVolume(m_savedVolume);
    }
    m_isPaused = isPause;
}

int AudioPlayer::write(const unsigned char *data, int datasize){
    if(!data || datasize<=0){
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if(!m_sink||!m_io){
        return false;
    }


    qint64 written = m_io->write(
        reinterpret_cast<const char*>(data),
        datasize
        );

    if (written < 0)
        return 0;

    return static_cast<int>(written);
}

int AudioPlayer::getFree(){
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sink) return 0;
    return m_sink->bytesFree();
}

void AudioPlayer::setVolume(qreal vol){
    std::lock_guard<std::mutex> lock(m_mutex);
    m_savedVolume = vol;
    if (!m_isPaused && m_sink) {  // 增加空判断
        m_sink->setVolume(vol);
    }
}
double AudioPlayer::getVolume(){
    return double(m_savedVolume);
}



AudioPlayer::AudioPlayer()
{
}


AudioPlayer::~AudioPlayer()
{
    this->close();
}
