#include "core/playercore.h"
#include "core/messageloopthread.h"
#include "core/messagequeue.h"
PlayerCore::PlayerCore() {
    setMessageHandler();
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
                // arg1=0, obj=std::string*(路径/URL)；成功后发 EVT_PREPARED

                break;
            case CMD_PLAY:
                // 开始/恢复播放（内部按状态机判断 PREPARED→STARTED / PAUSED→STARTED）

                break;
            case CMD_PAUSE:
                // STARTED→PAUSED

                break;
            case CMD_SEEK:
                // arg1=目标毫秒；完成后发 EVT_SEEK_DONE

                break;
            case CMD_STEP_FORWARD:
                // 暂停态逐帧前进

                break;
            case CMD_STEP_BACKWARD:
                // 暂停态逐帧后退（依赖 48 帧历史）

                break;
            case CMD_SET_RATE:
                // arg1=速率×100（100=1.0x，可选功能）

                break;
            case CMD_STOP:
                // 停止并释放解码/渲染线程，回到 IDLE

                break;
            case CMD_QUIT:
                // 退出消息循环（析构前）

                break;



            //  Core->UI
            case EVT_PREPARED:
                // arg1=时长ms, arg2=(宽<<16)|高

                break;
            case EVT_POSITION:
                // arg1=当前播放位置ms（100~200ms 一次，别每帧发）

                break;
            case EVT_STATE_CHANGED:
                // arg1=新状态（IDLE/INITIALIZED/PREPARING/PREPARED/STARTED/PAUSED/COMPLETED/STOPPED/ERROR）

                break;
            case EVT_DISABLEBTN:
                //关闭按钮

                break;
            case EVT_ABLEBTN:
                //开启按钮

                break;
            case EVT_PLAYNEXT:
                //当前视频播放完毕请求播放下一个

                break;
            case EVT_SEEK_DONE:
                // arg1=实际落点ms（seek 可能不精确，要对齐关键帧）

                break;
            case EVT_BUFFERING:
                // arg1=1 开始缓冲 / 0 结束（网络流才有意义）

                break;
            case EVT_COMPLETED:
                // 播到文件尾

                break;
            case EVT_ERROR:
                // arg1=错误码, obj=std::string*(错误文本)

                break;




            //DemuxThread -> PlayerCore
            case CTX_IS_EXIT:
                //arg1 = true/false   1/0

                break;
            case CTX_IS_PAUSE:
                //arg1 = 1/0

                break;
            case CTX_HAS_VIDEO:
                //arg1 = 1/0

                break;
            case CTX_HAS_AUDIO:
                //arg1 = 1/0

                break;
            case CTX_IS_SEEKING:
                //arg1 = 1/0

                break;
            case CTX_SEEKMS:
                //arg1 = ms

                break;
            case CTX_HAS_PLAYLIST:
                //arg1 = 1/0

                break;
            case CTX_IS_FRAME_STEP:
                //arg1 = 1/0

                break;
            case CTX_IS_STREAM_MEDIA:
                //arg1 = 1/0

                break;
            case CTX_SPEED:
                //arg1 = 10*speed

                break;


            default:

                break;
        }
    };

    m_loop->setMessageHandler(handler);
}
