#include "core/videorenderthread.h"
#include "platform/videowidget.h"
extern "C"
{
#include <libavutil/frame.h>
}
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <chrono>
#include <thread>

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

void VideoRenderThread::setPaintFrameCallBack(PaintFrame painter)
{
    m_painter = std::move(painter);
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
    //qDebug()<<"视频fps:"<<m_fps;
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

    /*
     * 只要线程对象存在就 join：
     * 线程可能已经因为队列 abort 自行退出（m_running 已经是 false），
     * 这时也必须 join，否则 std::thread 析构时会 std::terminate。
     */
    if (m_thread.joinable())
    {
        m_thread.join();
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

void VideoRenderThread::start()
{
    // 上一轮线程已经退出但没 join：先收掉，再起新线程
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    m_running.store(true);
    m_thread = std::thread([this]() {
        run();
        m_running.store(false);
    });
}

void VideoRenderThread::requestStep(int delta)
{
    m_stepReq.fetch_add(delta);
}

void VideoRenderThread::clearStepRequests()
{
    m_stepReq.store(0);
}


void VideoRenderThread::renderFrame(Frame* frame)
{
    if (!frame || !frame->m_frame || !m_painter)
    {
        return;
    }

    //提前复制一份（clone 只是引用计数+1），防止 setPaint 前数据失效
    AVFrame* renderFrame = av_frame_clone(frame->m_frame);

    if (!renderFrame)
    {
        return;
    }

    //回调播放
    m_painter(renderFrame);
}





void VideoRenderThread::run()
{
    //-----------
    //丢帧数统计
    // static long long loseFrameCount = -1;
    //读过的总帧数
    // static long long readenCount = -1;
    //-----------

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
        //是否暂停（逐帧也走这里：暂停渲染，但解码线程继续解码）
        if (m_isPause)
        {
            int req = m_stepReq.load();
            if (req > 0)
            {
                //下一帧
                Frame* frame = m_frameQueue->stepForward();
                if (frame)
                {
                    //成功取到帧才消费掉一次请求
                    m_stepReq.fetch_sub(1);
                    if (frame->m_frame)
                    {
                        pts.store(frame->m_frame->pts);
                        m_lastFramePts.store(frame->m_frame->pts);
                        renderFrame(frame);
                        //恢复播放时第一帧不要等待
                        m_firstFrame = true;
                    }
                    delete frame;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                else
                {
                    //解码还没跟上：请求留着，稍后重试
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;
            }
            else if (req < 0)
            {
                //上一帧
                FrameQueue::StepStatus st = FrameQueue::StepEmpty;
                Frame* frame = m_frameQueue->stepBackward(&st);
                if (frame)
                {
                    m_stepReq.fetch_add(1);
                    if (frame->m_frame)
                    {
                        pts.store(frame->m_frame->pts);
                        m_lastFramePts.store(frame->m_frame->pts);
                        renderFrame(frame);
                        m_firstFrame = true;
                    }
                    delete frame;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                else
                {
                    if (st == FrameQueue::StepAtBegin)
                    {
                        //已经在文件第一帧，这次请求直接作废
                        m_stepReq.fetch_add(1);
                    }
                    //StepNeedRefill / StepEmpty：回填或解码还没跟上，保留请求下一轮再试
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        //播放最后的帧数
        bool last = lastSome.load();
        if(last && m_frameQueue->size() <= 1  ){
            playDone.store(true);
        }
        //从FrameQueue获取Frame

        Frame* frame = m_frameQueue->getReadable();
        if (!frame || !frame->m_frame )
        {
            if (m_isExit || m_frameQueue->isAborted())
            {
                break;
            }
            continue;
        }

        //-----------
        // if(readenCount!=-1)
        //     readenCount++;
        //-----------

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

        //-----------
        // if(readenCount != -1){
        //     //五分钟的丢帧率
        //     if(loseTimer.elapsed() >= 5*60*1000 ){
        //         qDebug()<<"shown frames:" << readenCount;
        //         qDebug()<<"lose frames:"<< loseFrameCount;
        //     }
        //     readenCount++;
        // }
        //------------

        //视频-音频
        long long diff = videoPts - audioPts;
        qint64 compensate = 0;
        if (diff > SYNC_THRESHOLD) {
            compensate = qMin<qint64>((diff - SYNC_THRESHOLD) / 2, m_frameDurationMs);
            if(m_lastAudio_pts == audioPts){
                compensate = 0;
            }
        } else if (diff < -SYNC_THRESHOLD) {
            compensate = qMax<qint64>((diff + SYNC_THRESHOLD) / 2, -m_frameDurationMs);
        }

        m_lastAudio_pts = audioPts;
        qint64 renderStart = m_loopTimer.elapsed();
        //qDebug()<<"videoPts:"<<videoPts<<",audioPts:"<<audioPts<<",diff:"<<diff;
        //视频超前或者基本同步

        if (diff >= -SYNC_THRESHOLD )
        {
            m_maxFrameDrop = 5;
            renderFrame(frame);
            qint64 renderCost = m_loopTimer.elapsed() - renderStart;
            m_frameQueue->next();
            qint64 target = nowWall + m_frameDurationMs - renderCost + compensate;
            sleepUntil(target);
            if (diff > SYNC_THRESHOLD)
            {
                //-----------
                //正常播放后再开始统计丢帧率
                // if(loseFrameCount == -1 && readenCount == -1){
                //     loseTimer.start();
                //     qDebug()<<"开始计算5分钟内的丢帧数";
                //     loseFrameCount = 0;
                //     readenCount = 1;
                // }
                //-----------
            }
        }
        //else
        else{
            // 落后音频：丢帧
            //qDebug() << "落后音频：丢帧";
            //-----------
            // if(loseFrameCount!=-1){
            //     loseFrameCount++;
            // }
            //-----------
            if(m_maxFrameDrop>0){
                m_frameQueue->next();
                m_maxFrameDrop--;
            }else{
                m_maxFrameDrop = 5;
                renderFrame(frame);
                m_frameQueue->next();
            }

        }
    }
    qDebug() << "VideoRenderThread end...";
}

const qint64 SLEEP_GRANULARITY = 15;   // Windows 睡眠粒度
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
            std::this_thread::sleep_for(std::chrono::milliseconds(remain - SPIN_THRESHOLD_MS));
        }
        else
        {
            std::this_thread::yield();
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
