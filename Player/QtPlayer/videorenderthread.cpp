#include "videorenderthread.h"
#include "videowidget.h"
extern "C"
{
#include <libavutil/frame.h>
}
#include <QDebug>
#include <QMetaObject>

#include<algorithm>

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
    m_frameDurationMs = 1000.0 / m_fps;
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
                Frame* frame = m_frameQueue->getNextFrame();
                if (frame) {
                    pts.store(frame->m_frame->pts);
                    renderFrame(frame);
                    if(!m_isPlayPrevFrame){
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
        pts = videoPts;

        //音画同步
        long long audioPts = synpts.load();

        // 如果音频时钟还没有开始 || 音频先结束了视频得正常播放，此时diff>>100
        if (audioPts <= 0|| lastSome.load())
        {
            renderFrame(frame);
            m_frameQueue->next();
            sleepUntil(m_frameDurationMs);
            continue;
        }

        //视频-音频
        long long diff = videoPts - audioPts;
        //qDebug()<<"videoPts"<<videoPts<<"audioPts"<<audioPts<<"videoPts"<<diff;
        //视频超前
        if (diff > 50)
        {
            lastFrameWallMs = m_loopTimer.elapsed();
            sleepUntil(lastFrameWallMs + (m_frameDurationMs));
            continue;
        }
        //视频稍微超前或者基本同步
        if (diff >= -50)  //50
        {
            //显示这一帧
            renderFrame(frame);
            //消费Frame
            m_frameQueue->next();
            //下一帧
            lastFrameWallMs = m_loopTimer.elapsed();
            sleepUntil(lastFrameWallMs + (m_frameDurationMs/2));
            continue;
        }
        // 落后音频：丢帧
        m_frameQueue->next();
    }
    qDebug() << "VideoRenderThread end...";
}

void VideoRenderThread::sleepUntil(qint64 targetWallMs)
{
    qint64 remain = targetWallMs - m_loopTimer.elapsed();
    while (remain > 0 && !m_isExit) {
        msleep(std::min<qint64>(remain, 5));
        remain = targetWallMs - m_loopTimer.elapsed();
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

