#include "videorenderthread.h"
#include "videowidget.h"
extern "C"
{
#include <libavutil/frame.h>
}
#include <QDebug>
#include <QMetaObject>

#define SPIN_THRESHOLD_MS 2
#define SYNC_THRESHOLD 30

VideoRenderThread::VideoRenderThread(FrameQueue *frameQueue)
    :m_frameQueue(frameQueue)
    ,pts(0)
{

}


VideoRenderThread::~VideoRenderThread()
{
    stop();
}


void VideoRenderThread::setWidget(VideoWidget* widget)
{
    m_widget = widget;
}


void VideoRenderThread::setFps(double fps)
{
    if (fps <= 0)
    {
        fps = 25.0;
    }
    m_fps = fps;
    m_frameDurationMs = 1000.0 / m_fps / m_speed;
    pts = 0;
}

void VideoRenderThread::setPause(bool isPause)
{
    m_isPause.store(isPause);
}




void VideoRenderThread::setSerial(int serial)
{
    this->serial.store(serial);
}


void VideoRenderThread::stop()
{
    m_isExit = true;

    /*
     * 唤醒FrameQueue
     *
     * 如果RenderThread正卡在：
     *
     * getReadable()
     *
     * 那么abort可以让它马上返回。
     */
    if (m_frameQueue)
    {
        m_frameQueue->abort();
    }

    if (isRunning())
    {
        wait();
    }
}

void VideoRenderThread::resetFrameClock()
{
    m_firstFrame = true;
    m_lastFramePts.store(0);
}

void VideoRenderThread::restart()
{
    m_isExit = false;
    if (!isRunning()) start();
}


void VideoRenderThread::renderFrame(Frame* frame)
{
    if (!frame || !frame->m_frame || !m_widget)
    {
        return;
    }

    //提前复制一份，防止setpaint前就因为next()导致数据失效
    AVFrame* renderFrame = av_frame_clone(frame->m_frame);

    if (!renderFrame)
    {
        return;
    }

    QMetaObject::invokeMethod(
        m_widget,
        [widget = m_widget, renderFrame]()
        {
            widget->setPaint(renderFrame);
        },
        Qt::QueuedConnection
        );
}





void VideoRenderThread::run()
{

    qDebug() << "VideoRenderThread running...";

    if (!m_frameQueue)
    {
        qDebug() << "VideoRenderThread: FrameQueue is null!";
        return;
    }
    m_loopTimer.invalidate();
    m_loopTimer.start();
    qint64 lastFrameWallMs = 0;

    while (!m_isExit)
    {
        //是否暂停
        if (m_isPause)
        {
            //逐帧逻辑
            int cmd = m_FrameStepMode.exchange(0);
            if(cmd==1){
                //Frame* frame = m_frameQueue->getNextFrame();
                Frame* frame = m_frameQueue->getReadable();
                if (frame && frame->m_serial == serial) {
                    pts.store(frame->m_frame->pts);
                    renderFrame(frame);
                    if(!m_isPlayPrevFrame){
                        m_frameQueue->next();
                        m_frameQueue->next();
                    }
                    m_isPlayPrevFrame.store(false);
                }
            }else if(cmd == 2&&!m_isPlayPrevFrame){
                Frame* frame = m_frameQueue->getPrevFrame();
                if (frame) {
                    renderFrame(frame);
                    m_isPlayPrevFrame.store(true);
                }
            }
            msleep(5);
            continue;
        }

        //播放最后的帧数
        bool last = lastSome.load();
        if(last && m_frameQueue->size() <= 1  ){
            playDone.store(true);
        }
        //从FrameQueue获取Frame

        Frame* frame = m_frameQueue->getReadable();
        if (!frame)
        {
            if (m_isExit || m_frameQueue->isAborted())
            {
                break;
            }
            continue;
        }

        //检查serial
        if(frame->m_serial!=serial){
            m_frameQueue->next();
            continue;
        }

        // 获取视频PTS
        long long videoPts = frame->m_frame->pts;
        //供进度条使用
        pts.store(videoPts);

        //音画同步
        long long audioPts = synpts.load();

        qint64 nowWall = m_loopTimer.elapsed();

        //没有音频 || 音频先结束了视频得正常播放 , 视频做主时钟
        if ( !hasAudio || lastSome.load())
        {
            if (m_firstFrame) {
                // 第一帧：直接显示，不睡
                m_firstFrame = false;
                m_lastFramePts.store(videoPts);
                renderFrame(frame);
                m_frameQueue->next();
                continue;
            }
            // 这一帧的 pts 间隔（内容时间，毫秒）
            long long interval = videoPts - m_lastFramePts.load();
            // 倍速换算：内容时间 ÷ speed = 实际 wall 时间
            double speed = m_speed.load();
            if (speed <= 0) speed = 1.0;
            long long sleepMs = (long long)(interval / speed);
            // 防御：间隔异常（<=0 或超大）
            if (sleepMs < 0) sleepMs = 0;
            if (sleepMs > 500) sleepMs = m_frameDurationMs;   // 丢帧后的兜底，别睡死
            qint64 renderStart = m_loopTimer.elapsed();
            renderFrame(frame);
            qint64 renderCost = m_loopTimer.elapsed() - renderStart;
            m_frameQueue->next();
            // 目标 = 渲染开始时间 + 该睡的时间 - 渲染耗时
            qint64 target = m_loopTimer.elapsed() + sleepMs - renderCost;
            sleepUntil(target);
            m_lastFramePts.store(videoPts);
            continue;
        }


        //视频-音频
        long long diff = videoPts - audioPts;
        qint64 compensate = 0;
        if (diff > SYNC_THRESHOLD) {
            compensate = qMin<qint64>((diff - SYNC_THRESHOLD) / 2, m_frameDurationMs);
        } else if (diff < -SYNC_THRESHOLD) {
            compensate = qMax<qint64>((diff + SYNC_THRESHOLD) / 2, -m_frameDurationMs);
        }
        qint64 renderStart = m_loopTimer.elapsed();
        //qDebug()<<"videoPts"<<videoPts<<"audioPts"<<audioPts<<"diff"<<diff;
        //视频超前
        if (diff > SYNC_THRESHOLD || diff < -SYNC_THRESHOLD) //原50
        {
            renderFrame(frame);
            qint64 renderCost = m_loopTimer.elapsed() - renderStart;
            m_frameQueue->next();
            qint64 target = nowWall + m_frameDurationMs - renderCost + compensate;
            sleepUntil(target);

        }else
        //视频稍微超前或者基本同步
        if (diff >= -SYNC_THRESHOLD)  //50
        {
            renderFrame(frame);
            qint64 renderCost = m_loopTimer.elapsed() - renderStart;
            m_frameQueue->next();
            qint64 target = nowWall + m_frameDurationMs - renderCost + compensate;
            sleepUntil(target);
        }else{
            // 落后音频：丢帧
            //qDebug() << "落后音频：丢帧";
            m_frameQueue->next();
        }
    }
    qDebug() << "VideoRenderThread end...";
}

const qint64 SLEEP_GRANULARITY = 15;   // Windows msleep 粒度
void VideoRenderThread::sleepUntil(qint64 targetWallMs)
{
    while (!m_isExit)
    {
        qint64 now = m_loopTimer.elapsed();
        qint64 remain = targetWallMs - now;
        // 正常退出：时间到了，或 remain 异常（计时器失效导致的负数/超大值）
        if (remain <= 0 ) break;
        if (now < 0){
            m_loopTimer.invalidate();
            break;
        }
        // 额外防御：remain 不可能超过 targetWallMs（除非 targetWallMs 本身异常）
        if (remain > targetWallMs) break;
        if (remain > SLEEP_GRANULARITY + SPIN_THRESHOLD_MS)
        {
            msleep(remain - SPIN_THRESHOLD_MS);
        }
        else
        {
            QThread::yieldCurrentThread();
        }
    }
}


// 设置倍速：重算帧时长
// 2 倍速 = 每帧显示时间减半
void VideoRenderThread::setSpeed(double speed)
{
    if (speed <= 0) speed = 1.0;
    m_speed.store(speed);
    m_frameDurationMs = 1000.0 / m_fps / speed;
}

