# 播放器 + 推流器：知识点、坑与面试问答

> 这份文档是给我自己复习用的，目标只有一个：**能不看代码把整条链路讲清楚，并答得出"为什么"**。
>
> 读法建议：第一遍只读第 0 章（一页纸总览）→ 第二遍按"我讲不出的地方"跳读第 1~3 章 → 第三遍遮住答案做第 4 章的面试题 → 第 5 章自测清单全部能答上，就算有底了。
>
> 代码位置：`Player/QtPlayer/`（播放器）、`Player/QtPlayer/pusher/`（推流器）。

---

## 0. 一页纸总览

### 0.1 两个进程、两条链路

```
播放器（拉）： 文件/网络 ──demux──► 解码 ──► 帧队列 ──► 渲染(GPU) / 音频(声卡)
推流器（推）： 摄像头/麦克风 ──采集──► 帧队列 ──► 编码 ──► 包队列 ──► 封装 ──► 文件 / RTMP → SRS
                                                                              └──► 播放器再拉回来（闭环）
```

两个进程之间**只有 URL 一个契约**（`rtmp://`）。所以端到端实验就是：推流器推 → SRS → 播放器拉，然后测延迟。

### 0.2 线程模型对照（这是"心里有底"的核心）

| | 播放器 | 推流器 |
| --- | --- | --- |
| 1 | DemuxThread（独占 `AVFormatContext`，负责读包 + seek 响应） | 视频采集线程（独占采集 `AVFormatContext`） |
| 2 | VideoDecodeThread | 音频采集线程（就地编码，不排队） |
| 3 | VideoRenderThread（独占上屏 + 逐帧控制） | 视频编码线程（独占 `AVCodecContext`） |
| 4 | AudioThread（独占 `QAudioSink` 的喂数据） | 写出线程（独占输出 `AVFormatContext`，负责重连/trailer） |
| 队列 | `PacketQueue`（带 serial）+ `FrameQueue`（16 未来 + 48 历史 + 游标） | 帧队列（丢最旧）+ 包队列（落盘阻塞 / 推流丢包） |
| 背压方向 | **满了就等**（源头是文件，等得起，绝不能丢） | **帧队列满了就丢**（源头是设备，等不起）；包队列看场景 |

[重点] 这两张卡片的差别只有一句话：**播放器的队列是"资产"，推流器的队列是"实时流"**。理解了这句，两个项目的所有队列设计都能推出来。

### 0.3 三种时间基（所有时间戳问题都在这张图里）

```
采集源  ──pts 微秒 {1,1000000}──►  编码器  ──pts {1,fps}（音频 {1,sample_rate}）──►  容器流  ──st->time_base──► 网络/文件
                                    ▲                                                  ▲
                             av_rescale_q 换算                              必须在 write_header 之后现查现换算
```

实测（我的项目）：
- mp4 会被 muxer 改写成 **`1/15360`**（视频）、`1/48000`（音频）
- FLV/RTMP 是 **`1/1000`**（毫秒）

### 0.4 我项目里的实测数字（面试就靠这些）

| 指标 | 数值 | 怎么测的 |
| --- | --- | --- |
| 720p30 x264 veryfast 单进程 CPU | 约 80~190%（多线程可超 100%） | `--stats-csv` 里的 cpuPercent 列 |
| 3 秒 selftest 产物 | 视频 91 帧、音频 141 帧、232 个包、134 KB | `--selftest` + `ffprobe` |
| 20 秒长跑 | 视频 601 帧、音频 937 帧（=20×48000/1024）、0 丢帧、0 丢包、20.000 s | `--duration 20 --stats-csv` |
| 码率实测 | 目标 1000 kbps → 实测 985834 bps（偏差 1.5%） | `--cbr --bitrate 1000000` + `ffprobe` |
| RTMP 端到端 | 服务端收到 224 KB / 5.05 s，h264(High) + aac(LC) 两路齐全 | `ffmpeg -listen 1` 当服务端 |
| 断线重连 | 连接重置 `-10054` → 退避 500ms → 1000ms → 期间丢包 33→574、队列稳定在 32 | 推流中途杀服务端 |

[注意] 这些数字请自己在机器上复现一次再写进简历，别直接抄我这里的（环境不同数值会差）。

---

## 1. 通用底座（两个项目都吃这一套）

### 1.1 所有权与引用计数（所有内存问题的根源）

| 对象 | 创建 | 释放 | 关键语义 |
| --- | --- | --- | --- |
| `AVFormatContext` | `avformat_alloc_context` | `avformat_close_input` | [重点] `avformat_open_input` **失败时自己已经释放了**，别再 free 一次 |
| `AVCodecContext` | `avcodec_alloc_context3` | `avcodec_free_context` | |
| `AVFrame` | `av_frame_alloc` | `av_frame_free` | `av_frame_ref` 只加引用、`unref` 减引用，**不拷贝像素** |
| `AVPacket` | `av_packet_alloc` | `av_packet_free` | `av_packet_clone` 只加数据引用 |
| `AVDictionary` | `av_dict_set` | `av_dict_free` | |
| `SwsContext` / `SwrContext` | `sws_getContext` / `swr_alloc*` | `sws_freeContext` / `swr_free` | |
| `AVAudioFifo` | `av_audio_fifo_alloc` | `av_audio_fifo_free` | 写入会自动扩容 |

一句话原则：**你 alloc 的，你 free；`ref` 进来的，用 `unref` 还**。

### 1.2 send/receive 模式（FFmpeg 5+ 唯一的正确姿势）

```c
avcodec_send_packet(cc, pkt);          // 送一包
while (avcodec_receive_frame(cc, f) >= 0) { ... }   // 取帧，可能 0 帧也可能多帧
```

- `AVERROR(EAGAIN)`：还要更多输入才会出结果，**不是错误**
- `AVERROR_EOF`：已经 flush 干净，**不是错误**
- 编码侧对称：`send_frame` / `receive_packet`
- [重点] 一个包可能产出 0 帧（还没凑够）或多帧（B 帧重排），所以必须用 while 循环

### 1.3 三种时间基 + `av_rescale_q`

项目里出现时间基的地方只有三处，记住"谁是谁"就不会乱：
1. **流的** `AVStream::time_base`（容器/设备）
2. **编解码器的** `AVCodecContext::time_base`
3. **包的** pts/dts 单位 = 它所属那一层的时间基

换算永远用 `av_rescale_q(value, from_tb, to_tb)`，不要手算 `value * num / den`（会溢出、会丢精度）。

### 1.4 队列的两种背压语义

| 语义 | 用在 | 满了怎么办 | 为什么 |
| --- | --- | --- | --- |
| 阻塞（背压） | 播放器的 PacketQueue/FrameQueue；推流器的包队列（落盘） | 生产者等消费者 | 数据不能丢（丢帧=画面错乱，丢包=文件损坏） |
| 丢最旧 | 推流器的帧队列；包队列（推流） | 扔掉最旧的，收下新的 | 实时源等不起，留着过期数据只会让延迟越堆越大 |

### 1.5 中断回调（优雅退出的必备件）

```c
ctx->interrupt_callback.callback = cb;    // 返回非 0 = 立刻放弃当前 I/O
ctx->interrupt_callback.opaque   = &stopFlag;
```
- [重点] 必须在 `avformat_open_input` **之前**挂上，否则"打开设备/网络"这一步卡住就没法中断
- 用 `std::atomic<bool>` 做标志，从别的线程 set 是安全的
- 被打断时返回 `AVERROR_EXIT`

### 1.6 一个上下文只归一个线程

`AVFormatContext`、`AVCodecContext`、`SwsContext`、`SwsrContext` 都不是线程安全的。项目里靠"归属明确"而不是加锁保证正确性：每个线程独占自己那份，跨线程只传 `AVFrame`/`AVPacket` 的**引用**。

---

## 2. 播放器（Player）知识点

### 2.1 线程与数据流

```
DemuxThread ──AVPacket──► PacketQueue(带 serial) ──► VideoDecodeThread ──AVFrame──► FrameQueue ──► VideoRenderThread ──► VideoWidget(GPU)
                                                 └──► AudioThread ──► AudioPlayer(QAudioSink) ──► 声卡
```

- 只有 **DemuxThread** 碰 `AVFormatContext`；只有 **VideoRenderThread** 碰 GL；只有 **AudioThread** 喂声卡
- 所有 pts 统一用**毫秒**（这条不变量是整个项目最好用的一条约定）

### 2.2 FrameQueue：为什么是"三段的"

| 部分 | 结构 | 作用 |
| --- | --- | --- |
| 未来 | 固定 16 帧预分配环 | 解码器已经解出来、等着上屏的帧 |
| 历史 | `std::deque`，最多 48 帧 | 已经显示过的帧，**逐帧回退**靠它 |
| 游标 | `m_cursor` | 指向"当前屏幕上是哪一帧" |

三个必须记住的规则：
1. `stepForward` / `stepBackward` **只允许渲染线程调用**（否则游标和 GL 状态会打架）
2. 未来 + 历史**共用一把锁**（分开加锁必然死锁或撕裂）
3. 历史退到头时通过 `onNeedRefill` 回调让上层去 seek 回填（不能自己偷偷 seek）

### 2.3 PacketQueue 与 serial

- 有界队列，`push(pkt, serial, timeoutMs)`：超时返回 false，调用者自己处理包（不接管所有权）
- `serial` 是**seek 代数**：每次 seek 自增。队列里旧的 serial 包直接丢弃 —— 这是"seek 后旧数据不会串进来"的唯一保证
- `abort()` 唤醒所有等待者，用于退出

### 2.4 音画同步

主时钟策略：音频按声卡消耗推进、视频以它为参考对齐；差值超过阈值时丢帧或等待。播放器里所有跟"该显示哪一帧"相关的判断都基于同一套毫秒时间轴。

### 2.5 seek 的状态机

```
IDLE → SEEKING（记下目标、serial++、清队列）→ 等第一个关键帧 → 出帧 → 恢复播放/保持暂停
```
坑：seek 期间如果 UI 又点了暂停/逐帧，必须让新命令**覆盖**旧命令（否则状态机会卡在 SEEKING）。

### 2.6 渲染：yuv420p 三平面 `GL_RED` 上传

- `QOpenGLWidget` + 三个 `GL_RED` 纹理（Y/U/V），shader 里合成 RGB
- [重点] 上传前必须 `glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize)`（或按行拷贝），否则宽不是 4 的倍数时会错行
- 丢帧信箱：渲染线程只取"最新一帧"，来不及显示的直接丢（显示环节永远不该反压解码）

### 2.7 音频输出：`QAudioSink`

- 推模式：`QIODevice::write()` 由我们的 AudioThread 调，写不进去就等（这就是音频侧的背压）
- 暂停/恢复时要注意 sink 的 start/stop 与 resume 的配对，重复初始化会有爆音

### 2.8 已踩的坑（Linux 移植 + 跨线程）

| 坑 | 现象 | 原因 / 解法 |
| --- | --- | --- |
| GCC 不搜包含链 | `ui_player.h: fatal error: ctrlbar.h` | MSVC 会沿包含链回溯目录，GCC 不会 → `target_include_directories(... ${CMAKE_CURRENT_SOURCE_DIR})` |
| LP64 | `no declaration matches 'void DemuxThread::videoCallSeek(int64_t)'` | Linux 上 `int64_t == long`，Qt 槽签名必须写 `long long` |
| 大小写敏感 | `#include "VideoWidget.h"` 找不到 | Linux 文件系统大小写敏感，统一小写 |
| Wayland 拖拽/缩放 | 窗口拖不动；改成原生拖拽后又卡顿 | Wayland 不允许客户端 `move()`；`startSystemMove/Resize` 是**模态循环**会卡住播放 → 只在 Wayland 下用原生，X11/Windows 保留自绘 |
| 编译用的 Qt ≠ 运行的 Qt | 界面显示异常但代码没错 | `-DCMAKE_PREFIX_PATH` 指到了系统 Qt，实际跑的是 `~/Qt/6.11.2/gcc_64` |
| 跨线程碰 widget | 偶发卡顿/崩溃 | `VideoRenderThread` 直接调了 `m_widget->setPaint()`（内部是 `update()`）→ P2 用显式消息队列解决 |

### 2.9 已知问题

- Linux 上"打开文件"在 QtMultimedia 音频端点 SIGSEGV，未修（等 core 化把音频后端换掉）
- 性能数据（首帧 / seek P50·P95 / 常驻内存 / 2h 曲线）还没测，README 第 8 章是空表

---

## 3. 推流器（Pusher）知识点

### 3.1 五段流水线与数据变形

| 阶段 | 数据 | 时间戳 | 谁负责 | 线程 |
| --- | --- | --- | --- | --- |
| 采集 | `AVFrame`（原始/已解码） | 自行生成，微秒 | `IVideoSource` | 采集线程 |
| 转换 | `AVFrame` yuv420p | 不变 | `FrameConverter`（swscale） | 采集线程 |
| 编码 | `AVPacket` | 编码器 time_base | `IVideoEncoder` | 编码线程 |
| 封装 | `AVPacket` + 流 | 流 time_base | `IMuxer` | 写出线程 |
| 传输 | 字节 | — | `AVIOContext` | 写出线程 |

### 3.2 采集层：设备只是"另一种输入格式"

必须记住的六件事：
1. **`avdevice_register_all()` 必须调**（文件/网络的 demuxer 自动注册，设备不会）。它和已删除的 `av_register_all()` 不是一回事。
2. `av_find_input_format("dshow")` / `"v4l2"` / `"alsa"` 拿到设备后端
3. `avformat_open_input(&fmt, "video=设备名", ifmt, &opts)`：Windows dshow 的写法是 `video=名字` / `audio=名字`
4. **提议 ≠ 结果**：`video_size`/`framerate` 只是建议，`avformat_find_stream_info` 之后的 `codecpar` 才是真值，编码器必须用真值配
5. **设备给的常常是 MJPEG/H264**（压缩流），必须解码；YUY2/NV12 也统一走解码器（`rawvideo` 也有解码器）→ 代码只有一条路径
6. **时间戳自己造**：不同后端单位不同（v4l2 是 1µs、dshow 常是 100ns），设备时间戳还不可靠（官方有 `use_video_device_timestamps` 开关）

设备枚举：`avdevice_list_input_sources(ifmt, NULL, NULL, &list)`，注意 `AVDeviceInfo` 的两个字段：
- `device_name` = 备用名（`@device_sw_{GUID}`）
- `device_description` = 友好名（`AMD Privacy View camera`），命令行写这个

### 3.3 两种队列为什么策略不同

| 队列 | 策略 | 依据 |
| --- | --- | --- |
| 帧队列（采集 → 编码） | **丢最旧** | 采集是实时源，阻塞采集 = 延迟越堆越大 |
| 包队列（编码 → 写出） | 落盘：**阻塞**；推流：**丢包** | 文件必须完整；网络卡住时宁可丢也不能堆延迟 |

`DropQueue` 的 `close()` 语义要记牢：**不再收新数据，但存货必须能取干净** —— 这是"优雅退出"能写对的地基。

### 3.4 视频编码（libx264）

```c
cc->width/height/pix_fmt = AV_PIX_FMT_YUV420P;   // 必须和喂进来的帧一致
cc->time_base = {1, fps};  cc->framerate = {fps, 1};
cc->gop_size / max_b_frames / bit_rate;
cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;        // SPS/PPS 进 extradata，mp4/flv 都要
av_opt_set(cc->priv_data, "preset", "veryfast", 0);
av_opt_set(cc->priv_data, "tune", "zerolatency", 0);   // 低延迟：关掉前瞻缓冲
```

四个必答点：
1. **零拷贝喂帧**：`av_frame_ref` 做浅引用 → 只改 pts → `send_frame` → 立刻 `av_frame_free`（编码器内部已引用住数据）
2. **时间基换算**：输入是微秒，编码器是 `{1,fps}`，必须 `av_rescale_q`
3. **flush**：`send_frame(NULL)` + 循环 `receive_packet`，否则丢尾部若干帧
4. **B 帧**：会让 dts < pts、需要重排缓冲；实测 `--max-bframes 2` 能跑通，但推流默认给 0

CBR（近似）：`rc_min_rate = rc_max_rate = bit_rate` + `x264-params nal-hrd=cbr`。实测 1 Mbps 目标 → 985834 bps。

### 3.5 音频编码（AAC）多出来的两件事

1. **swresample**：设备给 s16（16 位整数、交错），AAC 只吃 **fltp（32 位浮点、平面）** → `swr_alloc_set_opts2` + `swr_convert`
   - 估算输出样本数时要加 `swr_get_delay()`，否则反复调用会漏样本
2. **AVAudioFifo**：AAC 每帧必须**正好** `frame_size`（1024）个采样，而设备每次给的样本数不固定 → 先攒够 1024 再编；flush 时补静音凑满

时间戳：编码器 time_base = `{1, sample_rate}`，所以 **pts 直接写"已产出的样本数"**，天然正确又单调。

### 3.6 封装（Muxer）

```c
avformat_alloc_output_context2(&out, NULL, "mp4"/*或"flv"*/, url);
avio_open2(&out->pb, url, AVIO_FLAG_WRITE, &interrupt_cb, &opts);   // opts 里放 rw_timeout（微秒）
avformat_new_stream + avcodec_parameters_copy(st->codecpar, par);
avformat_write_header(out, NULL);        // [重点] 这里之后 st->time_base 可能被改写
av_packet_rescale_ts(pkt, 编码器时间基, st->time_base);   // 所以必须现查
av_interleaved_write_frame(out, pkt);    // 接管并 unref 包，调用方别再 unref
av_write_trailer(out);                   // mp4 的 moov 箱在这一步写出去
```

三个坑：
1. **时间基改写**：mp4 把视频流改成 `1/15360`、FLV 用 `1/1000` —— 换算必须用 header 之后的 `st->time_base`
2. **`AVFMT_NOFILE`**：`null` 这类格式不需要 `avio_open`
3. **trailer 必须写**：不写 mp4 就没有 moov，文件直接打不开（我踩过，见 3.9）

### 3.7 推流与重连

- `rw_timeout`（微秒）：网络卡住超过它就返回错误，而不是无限期挂着
- 中断回调 + `abort()`：关程序时能立刻叫停阻塞的写
- **重连**：指数退避（500ms → 1s → 2s … 上限 15s），重连成功后**必须等一个视频关键帧**再继续写（否则 P 帧没有参考帧，画面花一片）；等待期间音频包也一起丢，防止音画错位
- **叫停的边界**：`abort()` 是给"卡住的数据写"用的；**落盘绝不能调**，否则连通 trailer 一起打断（这是真 bug，见 3.9）

### 3.8 停止顺序（七步，一步都不能换）

```
1) 采集源 requestStop()      打断阻塞的 av_read_frame（靠中断回调）
2) join 采集线程             此后不再产生新帧（音频线程退出前 flush 音频编码器）
3) 关帧队列 → join 编码线程   编码线程退出前 flush 视频编码器，尾包进包队列
4) 关包队列                  写出线程把存货写完
5) 等写出线程（推流给 2 秒，超时才叫停）
6) join 写出线程 → 它自己写 trailer
7) 关采集源
```

### 3.9 我踩过的四个真 bug（面试讲排查过程最加分）

1. **mp4 没有 moov、文件打不开**
   现象：`av_write_trailer` 报 `Immediate exit requested`，`ffprobe` 报 `moov atom not found`
   原因：`stop()` 无条件 `abort()`，把随后的 trailer 写也打断了
   修法：落盘绝不叫停；推流先给 2 秒排空，真卡住才叫停；`writeTrailer` 前撤销 abort 标志
2. **`--testsrc` 静默失效**
   原因：算了 `useSynthetic` 却只用在了 `--selftest` 分支里（变量未使用），结果偷偷去开了真实设备
3. **漏了 `avdevice_register_all()`**
   现象：`--list-devices` 报"找不到输入格式 dshow"
4. **重连后等关键帧被音频包清掉**
   原因：AAC 包有时带 `AV_PKT_FLAG_KEY`，把等待状态错误清除 → 从一个 P 帧开始写
   修法：只认 `stream_index == 0` 的视频关键帧

另外两个审计出来但没修的：设备中途改分辨率时 `FrameConverter` 不会重建（可能花屏/越界）；容器时长少一帧（最后一包 `duration=0`）。

---

## 4. 面试问答

> 用法：先遮住答案自己说一遍，再看答案对不对。答不上来的回到对应章节。

### 4.1 FFmpeg 基础

**Q1. `AVCodec` 和 `AVCodecContext` 的区别？**
前者是只读的"说明书"（全局一份），后者是有状态的实例（每个解码/编码器一份）。永远 open 的是 Context。

**Q2. `AVPacket` 和 `AVFrame` 的区别？**
包是压缩/编码后的数据，帧是解码后的原始像素/PCM。设备在 MJPEG 模式下给的是"包"，得先解码才是帧。

**Q3. `av_frame_ref` 和 `av_frame_clone` 有什么区别？**
两者都只加引用（零拷贝）。项目里我用 `ref` 到临时帧上改 pts，不改调用方的帧；`clone` 用在把包交给队列时。

**Q4. `av_read_frame` 返回负值怎么处理？**
`AVERROR(EAGAIN)`/`EOF` 不是错误；`AVERROR_EXIT` 是中断回调打的；其它才是真错。要区分对待，别一律退出。

**Q5. 为什么 `avformat_open_input` 失败后不需要 free？**
它内部已经释放了传入的 context（并在指针上置空），再 free 就是 double free。

**Q6. 解码循环为什么要 while 包 receive_frame？**
一个包可能出 0 帧（B 帧还没凑够）或多帧（重排），一次一问取不干净。

**Q7. C++ 里为什么不能用 `av_err2str`？**
它是依赖 C 复合字面量的宏，C++ 编不过；要用 `av_strerror(err, buf, sizeof(buf))`。

**Q8. `ch_layout` 和旧的 `channels` 字段？**
FFmpeg 5.x 起 `AVChannelLayout` 取代 `channels/channel_layout`，要用 `av_channel_layout_default/copy`，不要直接结构体赋值（自定义布局内部有指针）。

### 4.2 时间戳与同步

**Q9. 讲一下你项目里的三种时间基。**
流/设备（`AVStream::time_base`）、编解码器（`AVCodecContext::time_base`）、容器输出（`write_header` 之后的 `st->time_base`）。所有换算走 `av_rescale_q`。实测 mp4 视频被改成 1/15360、FLV 是 1/1000。

**Q10. 为什么不直接用设备给的时间戳？**
不同后端单位不同（v4l2 是 1µs、dshow 常 100ns），且设备时钟可能不准（官方专门给了 `use_video_device_timestamps` 开关）。推流只需要"单调、从 0 开始"的时间轴。

**Q11. 视频 pts 用"帧号×帧周期"有什么问题？**
设备实际帧率常是 29.97，若按 30 算，视频时间轴比真实时间快；音频按采样数走 → 半小时后音画漂移秒级。所以设备源改用单调时钟生成 pts。

**Q12. 音画同步你靠什么？**
两边时间戳同源于同一套单调时钟（视频用墙上时间、音频用样本计数），推流端靠 muxer 按 dts 交织；播放端以音频时钟为主时钟对齐视频。

**Q13. B 帧为什么会让 dts < pts？**
B 帧要参考后面的帧，编码顺序和显示顺序不同：dts 是解码/传输顺序，pts 是显示顺序。所以需要重排缓冲，封装时也必须按 dts 排。

**Q14. 播放器里 seek 之后时间戳怎么处理？**
靠 serial（seek 代数）：seek 时 serial++，队列里旧 serial 的包/帧直接丢；同时用 `AVStream::start_time` 归一化时间轴原点。

### 4.3 队列、线程与并发

**Q15. 为什么播放器的队列满了要阻塞，推流器的帧队列满了要丢？**
源头性质不同：播放器源头是文件，等得起，丢了就错；推流源头是设备，等不起，留旧数据只会让延迟越堆越大。

**Q16. 包队列为什么落盘时阻塞、推流时丢包？**
落盘要求文件完整（丢包=损坏）；推流要求实时（网络卡住时丢包比堆延迟好）。同一个 `DropQueue` 用两个策略。

**Q17. 你的队列 close() 是什么语义？**
停止接收新数据，但**存货仍能取干净**，取空之后 pop 才返回 false。这样收尾才能"先停生产、再消费干净、最后写 trailer"。

**Q18. 怎么保证跨线程安全？**
不共享可变状态，靠"归属"：每个 FFmpeg 上下文只被一个线程碰，跨线程只传带引用计数的 AVFrame/AVPacket；队列内部一把锁；共享计数用 `std::atomic`。

**Q19. 一个 FFmpeg 上下文能不能多线程同时用？**
不能。`AVFormatContext`/`AVCodecContext`/`SwsContext` 都不是线程安全的。

**Q20. 退出时怎么保证不卡死？**
中断回调 + `requestStop()`（打断阻塞的 `av_read_frame`）+ 有序 join；网络写还有 `rw_timeout` 和 `abort()` 兜底。

### 4.4 seek 与逐帧

**Q21. 逐帧回退怎么实现的？**
FrameQueue 保留最多 48 帧历史 + 游标；`stepBackward` 只把游标往前移（不重新解码）。历史退到头时通过 `onNeedRefill` 回调让上层 seek 回填。

**Q22. 为什么 `stepForward/stepBackward` 只允许渲染线程调？**
它们改的是"当前显示帧"这个状态，和 GL 上传必须在同一线程，否则会撕裂/闪烁。

**Q23. 未来队列和历史队列为什么用一把锁？**
两者之间有搬移（消费时把帧推进历史），两把锁必然出现"跨锁的中间状态"，容易死锁或看到不一致。

**Q24. seek 期间又来一个 seek 怎么办？**
serial 再自增，新的覆盖旧的；状态机只认最新目标。

### 4.5 编码与封装

**Q25. x264 参数里哪几个直接影响延迟？**
`preset`（越慢压缩越好但更吃 CPU）、`tune=zerolatency`（关前瞻/缓冲）、`gop_size`（关键帧间隔影响起播和 seek）、`max_b_frames`（B 帧引入重排延迟）、码率模式。

**Q26. `AV_CODEC_FLAG_GLOBAL_HEADER` 是干什么的？**
让 SPS/PPS（音频是 AudioSpecificConfig）作为 extradata 交给 muxer，而不是塞在码流里；mp4/flv 都需要。

**Q27. 为什么编码完必须 flush？**
编码器内部有缓冲（B 帧/前瞻），`send_frame(NULL)` 才会把压着的包全吐出来；不 flush 会丢尾部若干帧。

**Q28. 为什么 `av_interleaved_write_frame` 之后不能再 unref 包？**
它内部会接管并 unref（按 dts 交织）。再 unref 是重复操作（虽然对空的包无害，但容易误以为要手动管理）。

**Q29. 不写 trailer 会怎样？**
mp4 的 moov 箱在 trailer 里，不写文件打不开（我实测过 `moov atom not found`）。

**Q30. AAC 编码为什么需要 AVAudioFifo？**
AAC 每帧必须正好 1024 个采样，而设备每次给的样本数不固定（1024/480/882 都可能），必须攒够再编。

### 4.6 采集与推流

**Q31. 采集和播放的 API 差别在哪？**
只差"输入源"：`av_find_input_format("dshow")` + `avformat_open_input(设备名, ifmt, opts)`；之后 `av_read_frame` → 解码 → 转格式，和播放文件完全一样。

**Q32. 设备给的参数和我提议的不一样怎么办？**
`avformat_find_stream_info` 之后以 `codecpar` 为准，用真值去配编码器（否则会报尺寸/stride 不匹配）。

**Q33. 为什么 MJPEG 设备和 YUY2 设备可以走同一条代码路径？**
都统一走解码器（`rawvideo` 也有解码器），把"是否压缩"这个差异收进解码器，后面统一 swscale 转 yuv420p。

**Q34. 推流断线了怎么办？**
指数退避重连；重连成功后丢包直到**下一个视频关键帧**再继续写（P 帧没有参考帧会花屏）；等待期间音频也丢，防止音画错位。

**Q35. 怎么测端到端延迟？**
秒表法：手机同时拍推流端画面和播放端画面，看两个画面的时间差；也可以用画面里的时间码（我用的合成源可以画帧号）对照。

**Q36. 丢帧和丢包分别怎么统计、怎么看？**
`DropQueue` 自己计数（`dropped()`），每秒打进 `[stats]` 行和 CSV。推流时如果丢包持续增长，说明网络或写线程跟不上。

### 4.7 工程与调试

**Q37. 你怎么定位"时间戳错乱"这类问题？**
先看 muxer 打印的"输入时间基 / 输出时间基"，再用 `ffprobe` 看容器的 duration/frame rate 是否合理；两者对不上就顺时间基找。

**Q38. 怎么验证编码器输出是对的？**
和 `ffmpeg` 命令行同参数产物逐项对照（codec/profile/pix_fmt/帧率/时长），再做全片解码（`ffmpeg -v error -i x.mp4 -f null -`）确认无错。

**Q39. 怎么发现内存/资源泄漏？**
盯"谁 alloc 谁 free"的表格；关键是用"引用计数语义"检查 ref/unref 是否配对；多线程场景下检查退出路径是否 join。

**Q40. CI 里怎么跑 GUI 项目？**
`QT_QPA_PLATFORM=offscreen` + 只跑不依赖窗口的单测（FrameQueue、seek 状态机），CI 用 ubuntu-24.04 + apt 装 Qt6。

### 4.8 架构与演进

**Q41. 为什么要把核心和 Qt 解耦？**
一是为了多端（Android 上不能用 Qt Widgets），二是为了可测试（core 可以脱离 GUI 跑单测），三是为了让 UI 层只管交互、不阻塞在 FFmpeg 调用上。判据很客观：`core/` 里 grep 不到 `#include <Q...>`。
【已完成】这件事已经在一个目标上兑现：`op_pusher` 现在完全不依赖 Qt —— 日志用自写的 `pusher/log.h`（接口照 `QDebug` 抄，迁移只改函数名），参数解析/计时/CSV/信号处理都是标准库；`dumpbin` 里没有任何 `Qt6*.dll`。播放器那一半还在 Qt 上，是 P2 的剩余工作。

**Q42. ijkplayer 的架构你是怎么借鉴的？**
它把"状态机 + 消息队列"放在 core（`ffplayer.c` + `ff_ffmsg.c`），UI 层只是发命令、收事件（`IJKFFMoviePlayerController`）。我已经先拿推流器验证了"核心可以不依赖框架"这件事；播放器下一步就是把控制收敛成 `PlayerCore` 的状态机 + 显式消息队列，Qt 层只发命令。

**Q45. muxer 为什么用 `unique_ptr` 而不是 `shared_ptr`？**
因为叫停标志不再住在 muxer 里 —— 它由编排层持有（`std::atomic<bool> m_abortIo`），muxer 只借用它的指针，`stop()` 只需置位、不必安全地拿到 muxer 指针。于是 muxer 只被写出线程独占，裸所有权就够，`shared_ptr` + 原子 swap 那一套都不需要了。
（反面教材：早先那版把标志放在 muxer 里，`stop()` 就必须跨线程读指针，只能上 `shared_ptr` 保命。结论：**把状态搬到正确的层，比加锁/加引用计数更有效**。）

**Q46. 源码是 UTF-8（含中文注释）时，MSVC 要注意什么？**
必须加 `/utf-8`。坑在于：链 Qt 的目标能顺带拿到 Qt INTERFACE 选项里的 `/utf-8`，**不链 Qt 的目标拿不到** —— 于是 MSVC 按代码页 936 解 UTF-8 源码，先报 C4819，多字节序列里凑出引号/花括号后解析器跑偏，**报错最终出现在系统头文件里**（`ucrt/time.h`、`ctime`），看起来毫不相关。
解法：工程级 `if(MSVC) add_compile_options(/utf-8) endif()`，别让每个新目标各踩一次。

**Q47. 怎么保证 CI 能抓到本地测不到的问题？**
两条：一是**走用户真正会走的那条构建路径**做验证（我是手写 `cl.exe ... /utf-8` 验证的，所以 CMake 路径的坑一次没踩到，直到真机构建和 CI 才发现）；二是**改了 CMake 的依赖就必须同步改 CI 的依赖安装**（给采集加 `avdevice` 依赖时漏了 `libavdevice-dev`，CI 在配置阶段就死，报 `Package 'libavdevice' ... not found`）。
本机依赖全、环境杂，这类"构建环境不一致"的问题基本只有 CI 能抓到 —— 所以 CI 红不是坏事，是它在干活。

**Q43. 如果要支持 Android，哪些能复用？**
core（demux/解码/队列/同步）原样交叉编译；渲染换成 `ANativeWindow`+EGL（三平面 `GL_RED` 上传几乎照搬），音频换 Oboe，解码可加 MediaCodec 硬解后端。

**Q44. 要让延迟更低，你会从哪几个地方下手？**
编码端：zerolatency、关 B 帧、小 GOP、CBR、降低分辨率/码率；传输端：TCP→UDP 类协议（SRT/WebRTC）、减小发送缓冲；播放端：减小抖动缓冲、起播阈值、首帧不等关键帧堆积。

---

## 5. 自测清单（全部能讲清楚才算有底）

- [ ] 画出播放器 4 个线程和 2 个队列，标出每一跳的数据类型
- [ ] 画出推流器 4 个线程和 2 个队列，标出每一跳的时间基
- [ ] 说出三种时间基分别在哪、什么时候换算
- [ ] 解释"为什么播放器等、推流丢"（背压方向）
- [ ] 说出 `DropQueue::close()` 的语义以及它为什么让优雅退出变简单
- [ ] 背出停止顺序七步，并解释每一步为什么在这个位置
- [ ] 解释 `avdevice_register_all` 为什么必须调、`av_register_all` 为什么不用
- [ ] 说出"提议 ≠ 结果"，并说明编码器要拿哪份参数
- [ ] 解释零拷贝喂帧（`av_frame_ref` 后改 pts 的写法）
- [ ] 解释 AAC 为什么要 swresample + FIFO 两件套
- [ ] 解释 `write_header` 会改时间基，所以换算要现查
- [ ] 解释 trailer/moov 与"文件打不开"的关系
- [ ] 解释重连后为什么要等关键帧
- [ ] 讲出至少两个自己踩过的 bug 及排查过程
- [ ] 报出自己项目 3 个以上的实测数字并说明怎么测的

---

## 6. 已知问题与下一步

**已知问题（本次审计）**

| 项 | 影响 | 处理 |
| --- | --- | --- |
| 容器时长少一帧（最后一包 duration=0） | 外观 | 待定，3 行可修 |
| CSV 的 cpuPercent 在 Windows 上跳变 | 度量质量 | 改用整段平均或 `QueryProcessCycleTime` |
| CSV 列名 `packetsDropped` 实为"丢帧+丢包" | 易误解 | 改名待定 |
| `AVCodec::sample_fmts` 已 deprecated | 未来兼容 | 换 `avcodec_get_supported_config()` |
| 设备中途改分辨率时转换器不重建 | 可能花屏/越界 | 待修（约 5 行） |
| 播放器 Linux 上打开文件 SIGSEGV | 阻断 | 等 core 化换音频后端 |
| 性能数据未测 | 简历缺料 | P2 架构稳定后统一测 |

**下一步（P2：core 化 + 消息队列/状态机）**

1. 抽 `IVideoSink` / `IAudioSink`，渲染/音频线程只依赖接口
2. `core/drop_queue.h`（复用 pusher 的 DropQueue）
3. `core/message_queue.h` + `core/player_core.h`（状态机 + 命令），Qt 层改成"发命令/收事件"
4. 单测：状态机迁移表、消息队列、seek + serial
5. 根 CMake：`core/` 静态库 + `apps/player-qt` + `apps/pusher`

判据：`grep -r "#include <Q" core/` 为空；两个 app 的 UI 层都小于 500 行；Windows/Linux 双构建 + ctest 全绿。

---

## 7. 名词速查

| 名词 | 一句话 |
| --- | --- |
| GOP | 两个关键帧之间的帧数，越大码率越省、seek 越粗、起播越慢 |
| IDR | 立刻可解码的关键帧（H.264 里最干净的重启点） |
| PTS / DTS | 显示时间 / 解码（传输）时间；有 B 帧时 DTS < PTS |
| time_base | 时间戳的"单位"（`{1,fps}`、`{1,1000}`、`{1,15360}`） |
| extradata | 解码必需的带外信息：视频是 SPS/PPS，音频是 AudioSpecificConfig |
| moov | mp4 的元数据箱，写在 trailer 里，缺了文件打不开 |
| `AVFMT_NOFILE` | 这类格式不需要 `avio_open`（如 `null`） |
| EAGAIN / EOF | "还要更多输入" / "已经 flush 干净"，都不是错误 |
| serial | 播放器里标识"第几次 seek"的代数，用来作废旧数据 |
| 抖动缓冲 | 播放端为了抗网络抖动故意攒的一小段数据，越大越抗抖、延迟越高 |
| 背压 | 消费者慢时让生产者等，而不是无限缓冲 |
| 丢帧 / 丢包 | 帧=解码后画面（播放端丢帧、推流端丢采集帧）；包=压缩数据（网络侧丢包） |
