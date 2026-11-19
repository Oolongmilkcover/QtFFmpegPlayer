/*
 * ============================================================================
 *  op_player.h —— 播放内核的 C 接口（**草案 / 设计稿**，不是最终版）
 * ============================================================================
 *
 *  这份头文件放这里是给新仓库（SDK）当起点的：QtPlayer 自己**不编译、不链接**它。
 *
 *  为什么是 C 而不是 C++：
 *    鸿蒙只能经 NAPI 调 C 函数、安卓要走 JNI，C++ 对象（std::function / 虚表 /
 *    std::atomic）过不了这些边界；而且 C 接口才有 ABI 可言（同一份 .dll/.so
 *    能被不同编译器、不同版本的调用方使用）。
 *
 *  设计上的三个关键决定（都是为鸿蒙/安卓提前定的）：
 *    1. 音频是 **pull**（`op_audio_read`），不是 push 回调。
 *       鸿蒙 AudioRenderer / 安卓 AudioTrack 都是"设备来要数据"，让调用方在自己的
 *       音频线程里拉，最接近原生；SDK 内部仍然可以保留 push 的 IAudioOutput，
 *       只是外面包一层环形缓冲。
 *    2. `op_open` 支持 **fd**。鸿蒙 picker 给的是 URI、安卓是 content://，
 *       NDK 侧基本只能拿到文件描述符；只收路径的话两个平台都要在外面"先拷进沙箱"。
 *    3. 视频帧是 **借用视图**（`op_frame`），不跨边界传 AVFrame*。
 *       AVFrame 里有引用计数、有 FFmpeg ABI，不该泄给调用方。
 *
 *  还没有定稿的地方（标了 TODO，需要真机验证后再收敛）：
 *    - 事件结构体是否需要"复合事件"（比如一次 OPENED 同时带时长和宽高已够用）；
 *    - 音量是否要走 0~100 整数（当前是），以及是否要单独的 `op_set_mute`；
 *    - 是否需要 `op_audio_read` 之外再提供"设备延迟"接口（当前给了 latency_ms）。
 */

#ifndef OP_PLAYER_H
#define OP_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 导出宏 */
#if defined(_WIN32)
#  if defined(OP_BUILD_SHARED)
#    define OP_API __declspec(dllexport)
#  elif defined(OP_USE_SHARED)
#    define OP_API __declspec(dllimport)
#  else
#    define OP_API
#  endif
#else
#  if defined(OP_BUILD_SHARED)
#    define OP_API __attribute__((visibility("default")))
#  else
#    define OP_API
#  endif
#endif

/* --------------------------------------------------------------- 版本号 */
#define OP_API_VERSION_MAJOR 0
#define OP_API_VERSION_MINOR 1

/* --------------------------------------------------------------- 错误码 */
typedef enum op_result
{
    OP_OK              =   0,
    OP_ERR_INVALID_ARG =  -1,   /* 参数非法（空指针、越界值） */
    OP_ERR_OPEN_FAILED =  -2,   /* 打不开：文件不存在 / 格式不支持 / 网络失败 */
    OP_ERR_NO_STREAM   =  -3,   /* 打开了但没有可用的音视频流 */
    OP_ERR_UNSUPPORTED =  -4,   /* 当前平台/编码不支持 */
    OP_ERR_NOT_READY   =  -5,   /* 还没 open / 已经 stop */
    OP_ERR_BUSY        =  -6,   /* 上一个操作还没完成（比如 seek 进行中） */
    OP_ERR_IO          =  -7,   /* 读写设备/网络失败 */
    OP_ERR_INTERNAL    =  -8
} op_result;

/* --------------------------------------------------------------- 播放状态 */
typedef enum op_state
{
    OP_STATE_IDLE      = 0,     /* 还没打开任何文件 */
    OP_STATE_OPENING   = 1,     /* open 已受理，结果稍后以事件通知 */
    OP_STATE_READY     = 2,     /* 打开成功，首帧就绪，可以播 */
    OP_STATE_PLAYING   = 3,
    OP_STATE_PAUSED    = 4,
    OP_STATE_STOPPED   = 5,
    OP_STATE_ENDED     = 6      /* 播到结尾 */
} op_state;

/* --------------------------------------------------------------- 事件类型 */
typedef enum op_event_type
{
    OP_EVENT_OPENED     = 1,    /* duration_ms / width / height / has_video / has_audio / is_stream */
    OP_EVENT_POSITION   = 2,    /* position_ms：约 150ms 一次，供进度条用 */
    OP_EVENT_STATE      = 3,    /* state：状态机变化 */
    OP_EVENT_SEEK_DONE  = 4,    /* position_ms：实际落点（可能和请求值不同，要对齐关键帧） */
    OP_EVENT_BUFFERING  = 5,    /* has_video：1 开始缓冲 / 0 结束（网络流才有意义） */
    OP_EVENT_COMPLETED  = 6,    /* 播到文件尾 */
    OP_EVENT_ERROR      = 7     /* code + message */
} op_event_type;

/*
 * 事件：**扁平结构**（不用 union / 嵌套），两个原因：
 *   - FFI 友好：NAPI / JNI 逐字段读最简单，不用处理联合体；
 *   - 以后加字段不会改变已有字段的偏移，二进制兼容更容易保。
 * 哪个字段在哪个事件里有效，见上面每个枚举后面的注释。
 */
typedef struct op_event
{
    int32_t     type;           /* op_event_type */
    int32_t     state;          /* OP_EVENT_STATE */
    int32_t     code;           /* OP_EVENT_ERROR：op_result 或 FFmpeg 错误码 */
    int32_t     width;
    int32_t     height;
    int64_t     duration_ms;
    int64_t     position_ms;
    int32_t     has_video;      /* OPENED：有无视频；BUFFERING：1/0 */
    int32_t     has_audio;
    int32_t     is_stream;      /* 网络流（RTSP/RTMP/HLS...）时为 1 */
    const char *message;        /* 错误文本，**只在回调期间有效**，不归调用方释放 */
} op_event;

/* --------------------------------------------------------------- 视频帧 */
/*
 * 视频帧视图：**数据归 SDK 所有**，只在 `op_video_cb` 回调期间有效。
 * 调用方要么在回调里同步用完（比如直接 glTexSubImage2D 上传），
 * 要么自己 memcpy 走。回调返回后这块内存随时可能被复用。
 */
typedef struct op_frame
{
    const uint8_t *data[3];     /* Y / U / V 平面 */
    int32_t        linesize[3]; /* 每行字节数（可能 > 宽度，逐行上传时要用） */
    int32_t        width;
    int32_t        height;
    int64_t        pts_ms;      /* 显示时间戳（毫秒） */
    int32_t        format;      /* 0 = YUV420P（本版本只有这一种） */
} op_frame;

/* --------------------------------------------------------------- 回调 */
/* 事件回调：在 SDK 的消息循环线程上触发。禁止阻塞、禁止在回调里调 op_destroy/op_stop。 */
typedef void (*op_event_cb)(void *user, const op_event *ev);

/* 视频帧回调：在 SDK 的解码/渲染线程上触发，每帧一次。同样禁止阻塞。 */
typedef void (*op_video_cb)(void *user, const op_frame *frame);

/* 日志回调：level 0=debug 1=info 2=warn 3=error；line 只在回调期间有效。 */
typedef void (*op_log_cb)(void *user, int level, const char *line);

/* --------------------------------------------------------------- 句柄 */
typedef struct op_player op_player;

/* --------------------------------------------------------------- 打开参数 */
/*
 * 路径和 fd 二选一：fd >= 0 时优先用 fd（鸿蒙/安卓的文件选择器场景）。
 * fd 归调用方所有，SDK 不负责关闭它。
 */
typedef struct op_open_options
{
    const char *path;           /* 本地路径或 URL（rtsp:// rtmp:// http:// ...） */
    int32_t     fd;             /* 文件描述符；-1 表示不用 */
    int64_t     fd_offset;      /* fd 内的起始偏移，通常是 0 */
    int64_t     fd_size;        /* fd 可读长度，0 = 由 SDK 探测（普通文件） */
    int32_t     open_timeout_ms;/* 网络流打开超时，0 = 默认（当前 15s） */
} op_open_options;

/* ================================================================== */
/*  生命周期                                                           */
/*                                                                    */
/*  线程约定（很重要）：                                                */
/*   - op_create / op_destroy：在调用方的任意一个线程调用；op_destroy  */
/*     会 join 内部所有线程，返回后不会再有任何回调。                    */
/*   - **回调里不要调 op_destroy / op_stop**：那是自己等自己，会死锁。   */
/*   - 其它接口（play/pause/seek/...）都是异步投递，可以从任意线程调，   */
/*     包括从回调里调（这不是死锁，但会排队到下一个消息循环）。           */
/* ================================================================== */

OP_API op_player *op_create(void);
OP_API void       op_destroy(op_player *p);

/* 版本（major/minor 可为 NULL） */
OP_API op_result  op_version(int32_t *major, int32_t *minor);

/* 注册回调；cb 传 NULL 表示取消。user 会原样回传给回调。 */
OP_API op_result  op_set_event_cb(op_player *p, op_event_cb cb, void *user);
OP_API op_result  op_set_video_cb(op_player *p, op_video_cb cb, void *user);
OP_API op_result  op_set_log_cb  (op_player *p, op_log_cb   cb, void *user);

/* ================================================================== */
/*  打开 / 播放控制                                                     */
/* ================================================================== */

/*
 * 异步打开：立刻返回 OP_OK 表示"已受理"，真正结果通过
 * OP_EVENT_OPENED 或 OP_EVENT_ERROR 通知。重复 open 会先关掉上一个。
 */
OP_API op_result op_open(op_player *p, const op_open_options *opt);

OP_API op_result op_play (op_player *p);
OP_API op_result op_pause(op_player *p);
OP_API op_result op_stop (op_player *p);      /* 回到 IDLE，p 仍可复用 */

OP_API op_result op_seek_ms(op_player *p, int64_t ms);

/* 逐帧：dir 取 +1（下一帧）或 -1（上一帧，靠回填）。纯音频/流媒体返回 OP_ERR_UNSUPPORTED。 */
OP_API op_result op_step_frame(op_player *p, int32_t dir);
OP_API op_result op_exit_frame_step(op_player *p);

/* 倍速，用 ×10 的整数避免 C 接口里出现 double：5~20 表示 0.5x ~ 2.0x。 */
OP_API op_result op_set_speed(op_player *p, int32_t speed_x10);

/* 音量 0~100（线性）；具体增益曲线由各平台后端决定。 */
OP_API op_result op_set_volume(op_player *p, int32_t volume);

/* ================================================================== */
/*  音频：**拉模式**                                                    */
/*                                                                    */
/*  调用方在自己的音频线程里循环 op_audio_read()，把拿到的 PCM 写进     */
/*  平台的 AudioRenderer / AudioTrack。返回 0 表示暂时没数据（别忙转，   */
/*  睡 2~5ms 再试）。缓冲由 SDK 内部维护，并据此估算音频延迟。           */
/*                                                                    */
/*  PCM 格式：S16 交错（LRLRLR...），采样率/声道数由 op_set_audio_format */
/*  指定，默认 48000 / 2。                                             */
/* ================================================================== */

OP_API op_result op_set_audio_format(op_player *p, int32_t sample_rate, int32_t channels);

/* 返回实际读到的字节数；0 = 暂时没数据；< 0 = op_result 错误码 */
OP_API int32_t   op_audio_read(op_player *p, void *buf, int32_t capacity);

/* 缓冲里还有多少字节（调用方据此做回压 / 判断是否该等一下） */
OP_API int32_t   op_audio_pending(op_player *p);

/* 已缓冲但还没播的时长（毫秒）——音频时钟的延迟补偿量 */
OP_API int64_t   op_audio_latency_ms(op_player *p);

/* ================================================================== */
/*  查询（线程安全，任意线程可调）                                       */
/* ================================================================== */

OP_API int64_t   op_position_ms(op_player *p);
OP_API int64_t   op_duration_ms(op_player *p);
OP_API int32_t   op_state_of  (op_player *p);   /* op_state */
OP_API int32_t   op_has_video (op_player *p);
OP_API int32_t   op_has_audio (op_player *p);
OP_API int32_t   op_is_stream (op_player *p);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ============================================================================
 *  最小使用示例（伪代码，两种语言的形态）
 * ============================================================================
 *
 *  // ---- 桌面 / 任意 C++ 程序 ----
 *  op_player* p = op_create();
 *  op_set_event_cb(p, on_event, nullptr);
 *  op_set_video_cb(p, on_frame, nullptr);          // 回调里上传纹理
 *  op_set_audio_format(p, 48000, 2);
 *  op_open_options o = {0}; o.path = "a.mp4"; o.fd = -1;
 *  op_open(p, &o);
 *  // 音频线程：
 *  //   while (running) {
 *  //       int n = op_audio_read(p, buf, sizeof(buf));
 *  //       if (n > 0) device.write(buf, n); else sleep_ms(3);
 *  //   }
 *
 *  // ---- 鸿蒙（ArkTS + NAPI）----
 *  //   1) XComponent 拿到 OH_NativeWindow → eglCreateWindowSurface
 *  //   2) 视频回调里做三平面 glTexSubImage2D（和现在 VideoWidget::paintGL 同一套）
 *  //   3) AudioRenderer 的写回调里（JS 线程）调 op_audio_read，把 PCM 写回去
 *  //   4) 文件选择器给的 URI → fileIo.openSync 拿 fd → op_open(fd)
 *
 * ============================================================================
 *  和现有 C++ 内核的对应关系（迁出后照着接）
 * ============================================================================
 *
 *   op_create / op_destroy   →  new PlayerCore / delete PlayerCore（含 PlayerContext）
 *   op_set_event_cb          →  PlayerCore::setEventCallBack
 *   op_set_video_cb          →  DemuxThread::openFile 里的 PaintFrame 回调
 *   op_open                  →  CMD_OPEN（obj = 路径 + 回调 + IAudioOutput）
 *   op_play/pause/stop       →  CMD_PLAY / CMD_PAUSE / CMD_STOP
 *   op_seek_ms               →  CMD_SEEK
 *   op_step_frame            →  CMD_STEP_FORWARD / CMD_STEP_BACKWARD
 *   op_set_speed             →  CMD_SET_SPEED
 *   op_audio_read            →  新增：内部 IAudioOutput 换成环形缓冲，对外拉
 *   EVT_* → op_event         需要一层映射（arg1/arg2 的打包要拆成字段）
 */

#endif /* OP_PLAYER_H */
