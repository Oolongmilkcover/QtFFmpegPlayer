# QtPlayer · 基于 Qt 6 + FFmpeg 的多线程音视频播放器

> 我把这个桌面播放器从零写了一遍：没用 QMediaPlayer 那类成品链路，而是直接用 FFmpeg API 做
> 解封装 / 解码 / 音画同步 / OpenGL 渲染 / 音频输出，Qt 只负责界面和窗口。

| 项 | 内容 |
|---|---|
| 语言 | C++17 |
| 界面 | Qt 6.11（Widgets / Multimedia / OpenGL / OpenGLWidgets） |
| 媒体 | FFmpeg 8.1（avformat / avcodec / avutil / swresample / swscale / avfilter） |
| 渲染 | QOpenGLWidget + GLSL（YUV420P 三平面纹理，零拷贝上传） |
| 构建 | CMake ≥ 3.16，MSVC 2022 验证通过（代码里没有 Win32 API） |
| 平台 | 目前只在 Windows 上验证；架构上给跨平台留了口子 |

---

## 目录

- [1. 项目简介](#1-项目简介)
- [2. 功能特性](#2-功能特性)
- [3. 快速开始](#3-快速开始)
- [4. 架构设计](#4-架构设计)
- [5. 关键机制](#5-关键机制)
- [6. 目录结构](#6-目录结构)
- [7. 关键常量](#7-关键常量)
- [8. 性能数据](#8-性能数据)
- [9. 已知限制与后续规划](#9-已知限制与后续规划)
- [10. 参考资料与延伸阅读](#10-参考资料与延伸阅读)
- [开发问题与解决方案（面试复习笔记）](docs/problems-and-solutions.md)

---

## 1. 项目简介

我没用 QMediaPlayer 这类成品链路，而是直接基于 **FFmpeg API** 把解封装 / 解码 / 同步 / 渲染 / 音频输出
整条流程写了一遍，Qt 只负责界面和窗口系统。

动手前给自己定了三条原则：

1. **分层要清楚**：解封装、解码、渲染、音频输出各跑各的线程，中间用有界队列连起来；
2. **关键策略握在自己手里**：同步、丢帧、队列深度、历史深度、缓存上限都显式可调，不依赖库的黑盒行为；
3. **并发要能推理**：跨线程的状态只允许"归属明确的队列"和原子量两种，锁的粒度和顺序都写了注释约束。

---

## 2. 功能特性

> **演示（待补充）**：我打算放一张主界面截图 + 一段 10~15s 的 GIF（逐帧回退 / 倍速 / 滤镜切换）。
> 素材放 `docs/media/` 或走外链。

| 分类 | 功能 | 说明 |
|---|---|---|
| 播放 | 播放 / 暂停 / 停止 | 音频用 `QAudioSink`，暂停走 `suspend/resume` |
| 播放 | 进度条 Seek（异步） | UI 不阻塞；seek 请求交给 demux 线程执行 |
| 播放 | 快进 / 快退 5s | 基于当前显示帧 pts |
| 播放 | 倍速 0.5×–2.0× | 音频走 `atempo` 滤镜（不变调），视频同步调整帧间隔 |
| 播放 | 播放列表 | Dock 面板、上下集切换、双击播放 |
| 播放 | 拖拽文件 | 拖入即播放并加入列表 |
| 逐帧 | 下一帧 / 上一帧 | 暂停渲染、解码继续；**上一帧支持无限回退**（历史窗口 + 自动 seek 回填） |
| 画面 | OpenGL YUV 渲染 | YUV420P 三平面纹理 + GLSL，零拷贝上传 |
| 画面 | 滤镜 | 原色 / 灰度 / 反色 / 暖色 / 冷色（Shader uniform 切换） |
| 画面 | 全屏 / 无边框窗口 | 双击全屏、Esc 退出；自绘标题栏与边缘缩放 |
| 音频 | 音量调节 | 0–100，静音图标联动 |
| 音频 | 纯音频文件 | 自动黑屏，并关闭逐帧能力 |
| 输入 | 键盘快捷键 | 见下方表格 |
| 网络 | RTSP 参数 | `rtsp_transport=tcp` + `max_delay`，并支持中断回调安全退出 |

**快捷键**

| 按键 | 功能 | 按键 | 功能 |
|---|---|---|---|
| `Space` | 播放 / 暂停 | `←` / `→` | 快退 / 快进 5s |
| `F` | 下一帧 | `D` | 上一帧 |
| `↑` / `↓` | 音量 ±5% | `C` / `X` | 升速 / 降速 0.1× |
| `Esc` | 退出全屏 | 双击 | 全屏切换 |

---

## 3. 快速开始

### 3.1 依赖

- Qt 6.x（需 **Core / Gui / Widgets / Multimedia / OpenGL / OpenGLWidgets**）
- FFmpeg 开发包（含 `include/` 与 `lib/`，我用 8.1 验证）
- CMake ≥ 3.16，编译器需支持 C++17

### 3.2 构建

```bash
# 1) 配置（FFMPEG_PATH 有默认值，路径不同就用 -D 覆盖；Qt 用 CMAKE_PREFIX_PATH 指定）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="C:/Qt/6.11.0/msvc2022_64" \
      -DFFMPEG_PATH="C:/Program Files/ffmpeg/ffmpeg8.1"

# 2) 编译
cmake --build build --config Release

# 3) 运行
./build/Release/QtPlayer.exe     # Linux / macOS 下是 ./build/QtPlayer
```

### 3.3 打包给别人用（免安装绿色版）

程序用的是 shared 版 FFmpeg，**单独把 exe 拷走是跑不起来的**，必须把 Qt 与 FFmpeg 的运行库一起带上。
仓库里的 `scripts/build-release.ps1` 一步完成「Release 构建 → `windeployqt` 收集 Qt 依赖 → 拷 FFmpeg 运行库 → 压 zip」：

```powershell
# 默认输出 dist\QtPlayer-v2.0-win64\ 并生成同名 zip
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1

# 只探测路径与体积预估，不做任何构建
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 -DryRun

# 显式指定 Qt / FFmpeg / 版本号
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 `
    -QtDir C:\Qt\6.11.0\msvc2022_64 `
    -FfmpegPath "C:\Program Files\ffmpeg\ffmpeg8.1" `
    -Version v2.0
```

跑完直接双击 `dist\QtPlayer-v2.0-win64\QtPlayer.exe` 自测。

> **不要把这个目录提交进仓库。** 实测约 **250 MB**，其中 220 MB 是 FFmpeg 的 DLL
> （`avcodec-62.dll` 93 MB、`avfilter-11.dll` 90 MB）。二进制无法增量存储，每次重新构建都会往历史里
> 塞几百 MB 且永远删不掉；这两个文件也已逼近 GitHub 单文件 100 MB 的硬上限。
> 分发请走 **GitHub Release 附件**：新建 Release → 上传 zip → 用户解压双击即用。

手动打包等价于：

```bash
windeployqt --release --no-translations --compiler-runtime QtPlayer.exe
# 再把 FFmpeg bin 目录下的动态库拷到 exe 同级：
#   avcodec-*.dll  avformat-*.dll  avutil-*.dll  swresample-*.dll  swscale-*.dll  avfilter-*.dll
```

---

## 4. 架构设计

### 4.1 线程模型

```text
              ┌──────────────────────── DemuxThread ────────────────────────┐
              │  av_read_frame → 按 stream_index 分发                        │
              │  异步 seek / 逐帧回填（唯一持有 AVFormatContext）             │
              └───────┬──────────────────────────────────────┬──────────────┘
              视频包  │                                      │ 音频包
                      ▼                                      ▼
           ┌────────────────────┐                  ┌─────────────────────┐
           │ VideoDecodeThread  │                  │ AudioThread          │
           │ 解码 → 未来帧队列    │                  │ 解码线程 → 音频帧队列  │
           │ (own AVCodecContext)│                 │ 播放线程 → QAudioSink │
           └─────────┬──────────┘                  └──────────┬──────────┘
                     ▼                                        ▼
           ┌────────────────────┐                  ┌─────────────────────┐
           │ VideoRenderThread  │◀───── synpts ────│  音频时钟 (pts)       │
           │ 消费帧 / 计时 / 丢帧 │                  │ = 写入pts − 未播时长  │
           └─────────┬──────────┘                  └─────────────────────┘
                     ▼  Qt::QueuedConnection
           ┌────────────────────┐
           │    VideoWidget     │  QOpenGLWidget + GLSL（YUV420P）
           │ 丢帧邮箱 + 零拷贝    │
           └────────────────────┘
```

我把职责按"谁拥有什么资源"来分：

- **DemuxThread**：唯一持有 `AVFormatContext`，负责读包、分发、seek、逐帧回填；
- **VideoDecodeThread**：唯一持有视频 `AVCodecContext`，解码后写进未来帧队列；
- **VideoRenderThread**：唯一消费未来帧队列和历史帧队列，负责节奏控制、丢帧、逐帧步进；
- **AudioThread**：内部两个线程（解码 + 播放），同时向视频侧提供音频时钟。

### 4.2 三类缓冲

| 缓冲 | 实现 | 容量 | 作用 |
|---|---|---|---|
| 包队列 `PacketQueue` | `std::queue` + mutex/cond | ≥ 100 包 | 解复用与解码解耦；支持**带超时投递**与中止唤醒 |
| 未来帧队列 | 预分配**环形数组** | 16 帧 | 已解码未显示的帧；生产者=解码线程，消费者=渲染线程 |
| 历史帧队列 | `std::deque` + 光标下标 | 48 帧 | 已显示过的帧；逐帧回退用，耗尽时触发 seek 回填 |

> 为什么未来队列用环形、历史用 deque：我在 [开发问题与解决方案](docs/problems-and-solutions.md) 第 10 条里写过。
> 简单说，v1.0 我照 ffplay 的 `frame_queue` 做过 `keep_last / rindex_shown`，但它只能回退一帧，做不了"伪无限上一帧"。

### 4.3 一次正常播放的数据流

1. `DemuxThread::run()` 读到一个 `AVPacket`，按 `stream_index` 投递到视频 / 音频包队列；
2. 解码线程 `pop` 到包 → `avcodec_send_packet` → 循环 `avcodec_receive_frame`；
3. 解码帧把时间戳统一换算成**毫秒**后写入未来帧队列（`push()`）；
4. 渲染线程取帧，和音频时钟比较后决定"立刻显示 / 睡一会儿 / 丢帧"；
5. 帧被消费时进入历史队列（`next()`），同时克隆一份投递给 GUI 线程；
6. GUI 线程 `setPaint()` 只接管引用，`paintGL()` 直接从 `AVFrame` 上传纹理并绘制。

---

## 5. 关键机制

### 5.1 背压与限速

- 队列一律**有界**，容量写死（16 帧 / 100 包），满了生产者就等——这是我唯一的限速点；
- 但我不允许**控制线程**被数据堵住：包投递用 `push(pkt, serial, timeoutMs)` / `tryPush(...)`，
  **超时就返回 false**，把包暂存到 `m_pendingPkt`，下一轮先重试它。这样 demux 线程每轮最多阻塞 10ms，
  随时能响应 **seek / 逐帧回填 / 退出**；
- 我在 [开发问题与解决方案](docs/problems-and-solutions.md) 第 2、8 条里记了这两个教训的来龙去脉。

### 5.2 音视频同步（音频主时钟）

```
diff = videoPts - audioPts          // 单位 ms
SYNC_THRESHOLD = 30
```

| 情况 | 我采取的策略 |
|---|---|
| `diff > 30`（视频超前） | 显示当前帧后多睡 `(diff-30)/2`，上限一帧时长（慢慢补偿，不跳变） |
| `\|diff\| ≤ 30` | 正常节奏：睡到 `now + 帧时长 − 渲染耗时` |
| `diff < -30`（视频落后） | **直接丢帧**（`next()` 不渲染）追上音频 |
| 无音频 / 音频已结束 | 视频做主时钟，按**相邻帧 pts 差**定节奏 |

几个细节：

- 音频时钟取的是"**真实听到的位置**"：最近写入声卡帧的 pts 减去声卡未播时长
  （`QAudioSink::bufferSize() - bytesFree()` 换算），而不是"解码到哪了"；
- 倍速时内容时间轴和播放时间轴会分离，时钟换算要乘上当前速度（`atempo` 的输出按样本数重映射回内容 pts）；
- 播放末尾有尾帧阶段（`lastSome`）和播完标志（`playDone`）收尾，见问题文档第 7 条。

### 5.3 Seek（异步 + serial 版本号）

```
[GUI]  拖动进度条 → requestSeekMs(ms)：夹取范围、serial++、置 m_isSeeking、禁用按钮，然后立刻返回
[DMA]  run() 首判 m_isSeeking → doSeek()：
         暂停生产 → 清空包队列/帧队列 → avformat_flush
         → av_seek_frame(..., AVSEEK_FLAG_BACKWARD)   // 只能定位到关键帧
         → avcodec_flush_buffers
         → 读包 + 解码，直到第一帧 pts >= 目标 → 直接显示（repaintPts）
         → 把新 serial 同步给解码/渲染/音频线程，并复位重采样器与 atempo 滤镜
         → 恢复暂停状态、重新启用按钮
```

几个关键点：

- **serial 机制**：每次 seek 递增 `m_serial`，包和帧都带着 serial，消费端丢掉过期数据，
  这样 seek 后不会有旧帧残留；
- **帧级精确**：定位阶段必须**完整解码**，不能为了快跳过非参考帧；渲染线程记录的也是"**实际显示那一帧**"的 pts；
- **控制线程不阻塞**：见 5.1。

### 5.4 逐帧与无限回退

```
进入逐帧：只暂停"渲染"（解码继续跑）+ 暂停音频；记住进入前的暂停状态
下一帧  ：优先取历史里"已解码但还没显示到"的帧，其次消费未来帧队列
上一帧  ：光标在历史里往回走
到边界  ：请求 demux 向后 seek 一段 → 完整解码 → 只收集"严格早于边界"的帧 → 前插进历史
退出逐帧：按需 seek 回到当前显示帧（有音频时必须对齐，否则音画会错位）
```

- 历史容量 48 帧，单次回填最多保留 16 帧（`kRefillKeepFrames`）；
- 回填期间用**解码闸门**独占 `AVCodecContext`，避免和解码线程交叉 `send/recv`（问题文档第 12 条）；
- 历史满时的淘汰策略必须保证"回填有进展"，否则会空转（问题文档第 11 条）。

### 5.5 渲染

- `QOpenGLWidget` + 三个 `GL_RED` 纹理（Y / U / V），片元着色器做 YUV→RGB 和滤镜；
- **零拷贝上传**：`setPaint()` 只接管 `AVFrame` 引用，`paintGL()` 直接从 `frame->data[plane]` +
  `linesize` 上传（行宽等于平面宽度时一次 `glTexSubImage2D`，有 padding 时逐行上传）；
- **丢帧邮箱**：GUI 线程同一时刻只处理一帧（`beginPaint()/endPaint()`），GUI 忙的时候渲染线程直接丢帧，
  免得事件队列里堆一堆带着整帧 buffer 的投递（问题文档第 14、15 条）；
- 渲染线程和 GUI 线程之间用 `Qt::QueuedConnection` 投递，投递目标用 `QPointer` 保护。

### 5.6 音频链路

```
AVPacket → avcodec 解码 → swresample 重采样(S16/48k/2ch)
        → [1.0×] 直接入帧队列
        → [变速] abuffer → atempo → abuffersink，输出按样本数重映射回内容 pts
        → 播放线程 write 到 QAudioSink 的 QIODevice（push 模式）
```

- 音频时钟取自"声卡未播时长"，所以暂停必须用 `suspend()`、seek 后必须 `reset()` 丢掉残留；
- 变速时重建 `atempo` 滤镜并复位 `swr`，否则切换倍速会有爆音 / 错位。

---

## 6. 目录结构

```text
QtFFmpegPlayer/
├── README.md
├── docs/
│   └── problems-and-solutions.md    # 开发问题与解决方案（面试复习笔记）
├── scripts/
│   └── build-release.ps1            # 一键打包免安装绿色版（构建 + windeployqt + zip）
├── dist/                            # 打包输出（不入库）
└── Player/QtPlayer/
    ├── CMakeLists.txt
    ├── main.cpp
    ├── player.{h,cpp,ui}            # 主窗口：状态机、定时器、输入、全屏/缩放
    ├── demuxthread.{h,cpp}          # 解封装 + 异步 seek + 逐帧回填
    ├── decodethread.{h,cpp}         # 解码基类：包队列、send/recv、编解码器生命周期
    ├── videodecodethread.{h,cpp}    # 视频解码、seek 定位、历史回填解码
    ├── videorenderthread.{h,cpp}    # 帧消费、节奏控制、同步、丢帧、逐帧步进
    ├── audiothread.{h,cpp}          # 音频解码线程 + 播放线程 + 音频时钟
    ├── framequeue.{h,cpp}           # 未来帧环形队列 + 历史帧窗口（逐帧核心）
    ├── packetqueue.{h,cpp}          # 有界包队列（支持超时投递/中止）
    ├── videowidget.{h,cpp}          # QOpenGLWidget：YUV 渲染 + 滤镜 + 丢帧邮箱
    ├── audioplayer.{h,cpp}          # 音频输出抽象 + QAudioSink 实现（单例）
    ├── ctrlbar.{h,cpp,ui}           # 底部控制栏
    ├── topmenu.{h,cpp,ui}           # 顶部标题栏（无边框拖动）
    ├── playlist.{h,cpp,ui}          # 播放列表
    ├── myslider.{h,cpp}             # 支持点击跳转的进度条
    ├── Basic.shader                 # GLSL（YUV→RGB + 滤镜）
    ├── res.qrc / workBtnPNG/        # 资源与图标
    └── build/                       # 本地构建产物（已在 .gitignore 里）
```

---

## 7. 关键常量

| 常量 | 值 | 位置 | 含义 |
|---|---|---|---|
| `FrameQueue::MAX_QUEUE_SIZE` | 16 | `framequeue.h` | 未来帧队列容量（背压上限） |
| `FrameQueue::MAX_HISTORY_SIZE` | 48 | `framequeue.h` | 历史帧容量（能连续回退多少帧） |
| `kRefillKeepFrames` | 16 | `videodecodethread.cpp` | 单次回填保留的帧数（要比历史容量小得多） |
| `PUSH_TIMEOUT_MS` | 10 | `demuxthread.cpp` | 包投递超时（决定控制响应性） |
| `SYNC_THRESHOLD` | 30 | `videorenderthread.cpp` | 音画同步阈值（ms） |
| 回填跨度 | `(MAX_HISTORY_SIZE/2)·1000/fps`，夹在 `[500,5000]` ms | `demuxthread.cpp` | 回填时向后 seek 多远 |
| 包队列上限 | ≥ 100 | `packetqueue.h` | 单流包缓冲 |

**内存账**：1080p 8bit YUV420P 单帧 ≈ 3.11MB；未来 16 帧 + 历史 48 帧 = 64 帧 ≈ **200MB**。
历史深度就是"内存 ↔ 后退距离"的取舍，改 `MAX_HISTORY_SIZE` 就能换。

---

## 8. 性能数据

> 下面这些数字我自己测，测完填进来；还没测的位置标了「待填」。
> 内存类的是**设计上限估算**，用来说明取舍关系，实测以工具采样为准。

### 8.1 测试环境

| 项目 | 配置 |
|---|---|
| CPU | 待填（例：R9 5900x） |
| GPU / 驱动 | 6750GRE 12G |
| 内存 / 存储 | 16GB DDR4 4000 双通道 |
| 操作系统 | Windows 10 22H2 x64 |
| Qt / FFmpeg | Qt 6.11 / FFmpeg 8.1（shared build） |
| 构建 | MSVC 2022，Release，`/O2 /MD` |
| 测试片源 | ① 1080p30 H.264 8Mbps（GOP≈2s，B 帧=3）② 4K30 HEVC 20Mbps ③ 720p60 H.264 ④ 纯音频（MP3/FLAC） |
| 采样工具 | 任务管理器 / `typeperf` / `Get-Process` / 代码内 `QElapsedTimer` 打点日志 |

### 8.2 播放指标

| 指标 | 我怎么测 | 自定目标 | 实测 |
|---|---|---|---|
| 首帧时间 | `playFile()` 入口打点 → 第一帧真正上屏（`paintGL` 首次拿到有效帧） | < 500 ms | 待填 |
| 平均 CPU（1080p30） | 稳定播放后采样 60s：`typeperf "\Process(QtPlayer)\% Processor Time"`（多核可 >100%） | 待填 | 待填 |
| 峰值 CPU（4K30） | 同上 | 待填 | 待填 |
| 常驻内存（1080p） | 稳定后采样 60s 的私有工作集峰值：`(Get-Process QtPlayer).PrivateMemorySize64` | 待填 | 待填 |
| 长播内存曲线（2h） | 每 5min 采样一次私有工作集，看是否单调增长（泄漏判据） | 无增长趋势 | 待填 |
| 屏幕渲染 FPS | 程序内已有的每秒统计（`paintGL`）：正常播放 / 拖动窗口时分别记 | ≥ 片源帧率 | 待填 |
| 丢帧率 | 在渲染线程"落后音频丢帧"的分支加计数器，统计播放 5min 的 `丢帧数 / 总帧数` | < 1% | 待填 |
| 音画偏差 | 每秒采样一次 `diff = videoPts - audioPts`（代码里已有打印位置，取消注释即可），记录均值与 `\|diff\|` 最大值 | `\|diff\| < 50ms` | 待填 |

### 8.3 交互与逐帧指标

| 指标 | 我怎么测 | 自定目标 | 实测 |
|---|---|---|---|
| Seek 延迟 P50 / P95 | 随机 seek 30 次，打点"发出请求 → 目标帧上屏"（`requestSeekMs` → `repaintPts` 完成） | P95 < 300 ms | 待填 |
| 下一帧步进耗时 | 单次 `stepForward` 请求 → 帧上屏 | < 30 ms | 待填 |
| 上一帧步进耗时（命中历史） | 同上，不触发回填 | < 30 ms | 待填 |
| 回填耗时 | 打点 `doBackwardRefill()`：seek + 完整解码到边界 + 前插历史 | < 300 ms | 待填 |
| 上一帧端到端（首次跨边界） | 按下按键 → 画面更新（含等回填完成） | 待填 | 待填 |

### 8.4 优化前后对比

| 场景 / 指标 | 优化前 | 优化后 | 对应改动 |
|---|---|---|---|
| 1080p 播放 CPU | 待填 | 待填 | 去掉 `setPaint` 每帧 3MB 的 memcpy，改零拷贝直传 |
| 常驻内存 | 待填 | 待填 | 同上（去掉 `datas[]` 三块缓冲） |
| 拖动文件对话框时的渲染 FPS | 待填 | 待填 | 丢帧邮箱（在飞帧数 = 1） |
| 松手后画面"追帧" | 会把积压的帧补播一遍 | 无 | 同上 |
| 逐帧回退定位准确性 | 偏几帧 / seek 目标错误 | 帧级精确 | 完整解码定位 + 毫秒坐标系统一 |
| 长时间连续 seek 稳定性 | 偶发卡死 | 无卡死 | 控制线程非阻塞投递 + 背压环看门狗 |

### 8.5 配置取舍（历史深度 ↔ 内存 / 可回退距离）

> 估算公式：`内存 ≈ (16 + MAX_HISTORY_SIZE) × 单帧大小`，1080p 8bit YUV420P 单帧 ≈ 3.11 MB。

| 配置 | `MAX_HISTORY_SIZE` | 内存上限（估算） | 连续可回退 | 实测内存 |
|---|---|---|---|---|
| 省内存 | 16 | ≈ 100 MB | 16 帧 | 待填 |
| 默认 | 48 | ≈ 200 MB | 48 帧 | 待填 |
| 深度回退 | 96 | ≈ 350 MB | 96 帧 | 待填 |

---

## 9. 已知限制与后续规划

### 已知限制

1. **还是软解**：硬件解码（DXVA2 / D3D11VA / MediaCodec）我还没接，高码率 4K 下 CPU 占用偏高；
2. **时间轴原点**：进度条用"帧 pts / 容器 duration"算位置，没用 `AVStream::start_time` 归一化，
   某些起点不为 0 的流（如部分 TS）会有固定的显示偏移；
3. **VFR（可变帧率）**：回填跨度还是按 `avg_frame_rate` 估的，VFR 下不够准（可以用 `AVFrame::duration` 改进）；
4. **跨线程 UI 通知**：还有个别信号可能在 demux 线程发射（问题文档第 16 条）；
5. **字幕 / HDR 都没做**：字幕轨、10bit/HDR、以及色彩空间（BT.601/709、limited/full range）都还没处理；
6. **只验证过 Windows**：代码里没有 Win32 API，移植主要改构建和窗口细节。

### 接下来想做的

- [ ] 硬件解码（D3D11VA → MediaCodec/VAAPI）与零拷贝（Surface / DMA-BUF）
- [ ] 跨平台构建（CMake 参数化 + Linux 构建 + CI 矩阵）和 **Android 壳**（Surface/EGL + Oboe）
- [ ] 资源档位与编译期裁剪（队列深度 / 历史深度 / 滤镜开关），输出一张内存–性能实测表
- [ ] 把第 8 节的性能数据补齐
- [ ] 单元测试（FrameQueue 状态机、seek 状态机）并接进 CI
- [ ] 自适应缓冲（网络流弱网）、字幕、HDR / 色彩管理

---

## 10. 参考资料与延伸阅读

- [开发问题与解决方案（面试复习笔记）](docs/problems-and-solutions.md)：我开发中真实踩到的问题与排查过程
- FFmpeg 官方文档：`avformat` / `avcodec` / `avutil`（时间基、`av_seek_frame`、`AVFrame` 引用计数）
- ffplay（FFmpeg 自带参考播放器）：`frame_queue` 的环形预分配与 `keep_last` 设计
- mpv issue #4019：关于"逐帧后退为什么慢、以及缓存策略的取舍"
- GStreamer 设计文档 *Frame stepping*：步进事件与"后退需要 seek"的框架级约束
