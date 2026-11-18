#include "core/playercore.h"
#include "core/messageloopthread.h"
#include "core/messagequeue.h"
#include "core/playercontext.h"


PlayerCore::PlayerCore() {
    // 顺序要紧：先建队列 → 再建消息循环 → 把队列交给循环和 demux → 最后注册回调
    // （setMessageHandler() 内部会调 m_loop->setMessageHandler，m_loop 必须先存在）
    m_msgQue = new MessageQueue();

    m_loop = new MessageLoopThread();
    m_loop->setMessageQueue(m_msgQue);

    dt.setMessageQueue(m_msgQue);

    setMessageHandler();
}

PlayerCore::~PlayerCore()
{
    // 先同步停掉所有线程，再拆队列。
    stop();

    // dt 是值成员，析构在本体之后；这里把它的队列指针摘掉，
    // 免得它的析构再往即将释放的队列里推消息。
    if (m_loop) {
        delete m_loop;
        m_loop = nullptr;
    }
    dt.setMessageQueue(nullptr);
    delete m_msgQue;
    m_msgQue = nullptr;
}



void PlayerCore::setMessageHandler()
{
    // 定义消息处理回调。外部（如 PlayerCore）注册这个回调，决定怎么处理消息
    //using MessageHandler = std::function<void(PlayerMessage& msg)>;
    MessageLoopThread::MessageHandler handler = [this](PlayerMessage& msg){
        int what = msg.what;
        switch(what){

            //UI->Core
            case CMD_OPEN:
                {
                    //obj=CMD_OPEN_OBJ*（路径 + 视频帧回调 + 音频输出），定义见 core/messagequeue.h
                    auto *op = static_cast<CMD_OPEN_OBJ*>(msg.obj);
                    m_playerctx->setSetPlayingName(op->name);
                    if(!dt.openFile(op->name.c_str(), op->cb, op->out)){
                        postMessage(getErrorMsg("dt.openFile failed"));
                        return;
                    }
                    dt.start();
                    break;
                }

            case CMD_PLAY:
                // 开始/恢复播放（内部按状态机判断 PREPARED→STARTED / PAUSED→STARTED）
                break;
            case CMD_PAUSE:
                {
                    // STARTED→PAUSED  CMD_PLAY废弃了，这里用一条路走暂停
                    dt.setPause(msg.arg1==1?true:false);
                    break;
                }
            case CMD_SEEK:
                {
                    //用arg1和arg2拆分int64为高位和低位 arg1=目标毫秒；完成后发 EVT_SEEK_DONE
                    //注意：不能在 int 上 << 32（C4293 未定义行为），先转 uint64 再拼，
                    //高 32 位按无符号解释，拼完再整体转回有符号 int64。
                    const uint64_t hi = static_cast<uint32_t>(msg.arg1);
                    const uint64_t lo = static_cast<uint32_t>(msg.arg2);
                    int64_t posMs = static_cast<int64_t>((hi << 32) | lo);
                    dt.seekToMs(posMs);
                    break;
                }
            case CMD_STEP_FORWARD:
                {
                    // 暂停态逐帧前进
                    if(!dt.stepNextFrame()){
                        postMessage(getErrorMsg("stepNextFrame failed"));
                    };
                    break;
                }
            case CMD_STEP_BACKWARD:
                {
                    // 暂停态逐帧后退（依赖 48 帧历史）
                if (!dt.stepPrevFrame()){
                    postMessage(getErrorMsg("stepPrevFrame failed"));
                }
                    break;
                }
            case CMD_STEP_STOP:
                    dt.endFrameStep();
                    break;
            case CMD_SET_SPEED:
                {
                    // arg1=速率×10（10=1.0x，）
                    double speed = static_cast<double>(msg.arg1) / 10.0;
                    dt.setSpeed(speed);
                    break;
                }
            case CMD_STOP:
                {
                    // 停止并释放解码/渲染线程，回到 IDLE
                    dt.close();
                    break;
                }
            case CMD_QUIT:
                {
                    // 退出消息循环（析构前）

                    break;
                }



            //  Core->UI
            case EVT_PREPARED:
                // arg1=时长ms, arg2=(宽<<16)|高
            case EVT_POSITION:
                // arg1=当前播放位置ms（100~200ms 一次，别每帧发）
            case EVT_STATE_CHANGED:
                // arg1=新状态（IDLE/INITIALIZED/PREPARING/PREPARED/STARTED/PAUSED/COMPLETED/STOPPED/ERROR）
            case EVT_DISABLEBTN:
                //关闭按钮
            case EVT_ABLEBTN:
                //开启按钮
            case EVT_PLAYNEXT:
                //当前视频播放完毕请求播放下一个
            case EVT_SEEK_DONE:
                // arg1=实际落点ms（seek 可能不精确，要对齐关键帧）
            case EVT_BUFFERING:
                // arg1=1 开始缓冲 / 0 结束（网络流才有意义）
            case EVT_COMPLETED:
                // 播到文件尾
            case EVT_ERROR:
                // arg1=错误码, obj=std::string*(错误文本)
                /*
                 * 注意 obj 的生命周期：这里只是把消息转给 UI，**不转移所有权** ——
                 * MessageLoopThread::loop() 在本回调返回后会统一 free_l(obj)。
                 * 所以 UI 侧只能当场把内容拷走（Player::onEVT_ERROR 里就是
                 * QString::fromStdString 拷一份），不能存指针。
                 */
                m_callBack(msg);
                break;




            //DemuxThread -> PlayerCore
            case CTX_IS_EXIT:
                //arg1 = true/false   1/0
                m_playerctx->setIsExit(msg.arg1==1?true:false);
                break;
            case CTX_IS_PAUSE:
                //arg1 = 1/0
                m_playerctx->setIsPause(msg.arg1==1?true:false);
                break;
            case CTX_HAS_VIDEO:
                //arg1 = 1/0
                m_playerctx->setHasVideo(msg.arg1==1?true:false);
                break;
            case CTX_HAS_AUDIO:
                //arg1 = 1/0
                m_playerctx->setHasAudio(msg.arg1==1?true:false);
                break;
            case CTX_IS_SEEKING:
                //arg1 = 1/0
                m_playerctx->setIsSeeking(msg.arg1==1?true:false);
                break;
            case CTX_SEEKMS:
                //arg1 = ms
                m_playerctx->setSeekMs(msg.arg1);
                break;
            case CTX_HAS_PLAYLIST:
                //arg1 = 1/0
                m_playerctx->setHasPlayList(msg.arg1==1?true:false);
                break;
            case CTX_IS_FRAME_STEP:
                //arg1 = 1/0
                m_playerctx->setIsFrameStep(msg.arg1==1?true:false);
                break;
            case CTX_IS_STREAM_MEDIA:
                //arg1 = 1/0
                m_playerctx->setIsStreamMedia(msg.arg1==1?true:false);
                break;
            case CTX_IS_AUDIO_MEDIA:
                //arg1 = 1/0
                m_playerctx->setIsAudioMedia(msg.arg1==1?true:false);
                break;
            case CTX_SPEED:
                //arg1 = 10*speed
                m_playerctx->setSpeed(static_cast<double>(msg.arg1*1.0/10.0));
                break;


            default:
                // 未识别的 what：直接忽略（obj 仍由消息循环统一释放）
                break;
        }
    };

    m_loop->setMessageHandler(handler);
}

void PlayerCore::setEventCallBack(EventCallback callBack)
{
    m_callBack = std::move(callBack);
}

void PlayerCore::postMessage( PlayerMessage&& msg)
{
    m_loop->postMessage(std::move(msg));
}

void PlayerCore::postMessage(int what, int arg1, int arg2, void *obj, std::function<void (void *)> free_l)
{
    m_loop->postMessage(what,arg1,arg2,obj,free_l);
}



void PlayerCore::start()
{
    m_stopped = false;
    //复位整体退出标志：上一轮 stop() 会把它置 true（见下面 stop() 里的说明）
    if (m_playerctx) m_playerctx->setIsExit(false);
    m_loop->start();
}

void PlayerCore::stop()
{
    // 幂等：析构里还会再调一次
    if (m_stopped) return;
    m_stopped = true;

    /*
     * 0) 先把"整体退出"标志置上，再停消息循环。
     *
     * 消息循环线程可能正卡在 CMD_OPEN 的 avformat_open_input 里（网络流：DNS、
     * TCP 建连、协议握手都可能耗几秒到几十秒）。DemuxThread 的中断回调
     * （shouldExit() → ctx.getIsExit()）是 FFmpeg 唯一能打断这些阻塞的入口，
     * 所以必须在 join 之前把它置起来 —— 只置 dt 自己的 m_isExit（close() 里做）
     * 要等 join 之后才轮到，那时已经晚了。
     */
    if (m_playerctx) m_playerctx->setIsExit(true);

    // 1) 先停消息循环（内部 join）—— 停完就不会再有回调打进 UI 线程
    if (m_loop) m_loop->stop();

    // 2) 再关 demux 及其下面的解码/渲染/音频线程（join）
    dt.close();
}

void PlayerCore::setPlayerContext(PlayerContext *ctx)
{
    m_playerctx = ctx;
    dt.setPlayerContext(ctx);
}
