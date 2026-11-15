#include "core/playercore.h"
#include "core/log.h"
#include "core/messageloopthread.h"
#include "core/messagequeue.h"
#include "core/playercontext.h"


PlayerCore::PlayerCore() {
    m_msgQue = new MessageQueue();
    setMessageHandler();
    m_loop->setMessageQueue(m_msgQue);
    dt.setMessageQueue(m_msgQue);
}



void PlayerCore::setMessageHandler()
{
    // 定义消息处理回调。外部（如 PlayerCore）注册这个回调，决定怎么处理消息
    //using MessageHandler = std::function<void(PlayerMessage& msg)>;
    MessageLoopThread::MessageHandler handler = [this](PlayerMessage& msg){
        bool needFree = true;
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
            case CMD_SET_SPEED:
                {
                    // arg1=速率×10（10=1.0x，）
                    double speed = msg.arg1 / 10.0;
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
                m_callBack(msg);
                needFree = false;
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
            case CTX_SPEED:
                //arg1 = 10*speed
                m_playerctx->setSpeed(static_cast<double>(msg.arg1*1.0/10.0));
                break;


            default:

                break;

                if(needFree){

                }
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
    m_loop->start();
}

void PlayerCore::setPlayerContext(PlayerContext *ctx)
{
    m_playerctx = ctx;
}
