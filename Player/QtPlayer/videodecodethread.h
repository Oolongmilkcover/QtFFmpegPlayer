/*
解码视频
同步音频时钟
按原视频帧率显示
传给 VideoWidget
*/

#ifndef VIDEODECODETHREAD_H
#define VIDEODECODETHREAD_H

#include "decodethread.h"



#include <QObject>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
class VideoWidget;
class VideoRenderThread;
class VideoDecodeThread : public DecodeThread
{
    Q_OBJECT
public:
    //同步时间，由外部传入
    void setSynpts(long long synpts);

    explicit VideoDecodeThread(int frameSize = 100 , bool keep_last = true);
    ~VideoDecodeThread();
    //
    bool open(VideoWidget* widget,int width,int height,AVStream* videoStream);

    //给seek做的函数，如果没到达指定pos就释放，到了就显示并释放
    bool repaintPts(AVPacket *pkt, int64_t seekpts,int serial);

    //暂停（解码线程 + 渲染线程）
    void setPause(bool isPause);
    //只暂停渲染，解码继续
    void setRenderPause(bool isPause);

    //paint
    void paint(AVFrame* frame);

    void run() override;

    //设置“还有视频未播放”
    void setLastSome(bool lastSome);

    long long getVideoRenderPts();

    //设置fps
    void setFps(AVStream* videoStream);
    //取fps
    double getFps() const;

    //有没有音频
    void setHasAudio(bool flag);

    // 完全停止并释放解码器
    void close();

    // 只停线程+清队列
    void stopAndClear();

    // 重启解码线程
    void restart();

    // ==================== 逐帧 ====================
    // 请求逐帧：delta = +1 下一帧，-1 上一帧
    void requestStep(int delta);
    // 清掉所有还没执行的逐帧请求
    void clearStepRequests();
    // 当前显示帧是否就是队列里的最新帧（退出逐帧时判断能否无缝续播）
    bool cursorAtNewest();

    // ==================== 历史回填 ====================
    // 逐帧回退到历史最旧帧时需要向后补一段：由 demux 线程调用

    // 清掉待解码数据（保留历史帧）
    void clearForRefill();
    // 从当前(已 seek 到 boundary 之前)位置解码到 boundaryMs，收集更早的帧回填历史
    // readPkt 由 demux 提供，返回视频包（内部释放非视频包），返回 nullptr 表示结束
    bool refillBackward(int64_t boundaryMs, int serial,
                        const std::function<AVPacket*()>& readPkt);
    // 回填收尾
    void finishRefill(bool gotFrames);

    void setSerial(int serial);

    bool getPlayDone();

    void setSpeed(double speed);

    //历史耗尽，需要 demux 线程向后 seek 回填
    std::atomic<bool> needRefill{false};
    //回填的上界（历史最旧帧的 pts，毫秒）
    std::atomic<long long> refillBoundaryPts{-1};

private:
    std::atomic<bool> m_isPause = false;
    VideoWidget* m_widget = nullptr;
    //视频渲染线程  framequeue消费者
    VideoRenderThread *m_videoRenderThread = nullptr;
    AVStream* m_videoStream = nullptr;
    std::mutex m_viMutex;

    //逐帧回填时独占解码循环，避免和解码线程交叉 send/recv
    std::mutex m_decodeGate;
    std::atomic<bool> m_stepDecoding{false};
    //回填期间会临时把 skip_frame 调成完整解码，这里保存原值用于恢复
    int m_originalSkip = 0;

};

#endif // VIDEODECODETHREAD_H
