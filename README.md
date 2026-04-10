# 基于 Qt 6.11 + FFmpeg 8.1 的多线程音视频播放器（支持音画同步 / Seek / 实时播放）

---

## 目录

- [项目简介](#项目简介)
- [项目亮点](#项目亮点)
- [功能特性](#功能特性)
- [项目结构](#项目结构)
- [技术架构](#技术架构)
- [环境依赖](#环境依赖)
- [构建方法](#构建方法)
- [使用说明](#使用说明)
- [核心模块说明](#核心模块说明)
- [音视频同步原理](#音视频同步原理)
- [常见问题](#常见问题)
- [未来优化方向](#未来优化方向)
- [设计思考](#设计思考)
- [开发难点与解决方案](#开发难点与解决方案)

---

## 项目简介

QtPlayer 是一个基于 Qt 6.x 和 FFmpeg 8.1 开发的桌面视频播放器，实现了：

- 视频解封装（Demux）
- 音视频解码（Decode）
- 音频播放（QAudioSink）
- 视频渲染（QWidget）
- 音视频同步（Audio Clock）

---

## 项目亮点

- 基于 **Qt 6.11 + FFmpeg 8.1**，兼容最新 API（非过时实现）
- 基于 **多线程生产者-消费者模型** 构建播放器核心架构，实现 Demux / Decode / Render 解耦
- 设计并实现 **音频时钟驱动的音视频同步机制（Audio Clock）**
- 多线程解耦架构（解封装 / 音频 / 视频独立线程）
- 支持 **无阻塞 Seek（拖动进度条不卡 UI）**
- 使用 Qt6 新音频接口 **QAudioSink**
- 自实现线程安全队列（mutex + queue）
- 支持 **RTSP 网络流参数配置（TCP / 低延迟）**
  
---

## 功能特性

- 支持常见视频格式播放（依赖 FFmpeg）
- 音频播放（Qt6 QAudioSink）
- 播放 / 暂停
- 拖动进度条 Seek 且UI不阻塞
- 双击全屏

---

## 项目结构
```text
QtPlayer/
├── main.cpp
├── player.* # 主界面
├── player.ui
├── demuxthread.* # 解封装线程
├── decodethread.* # 解码基类
├── audiothread.* # 音频线程
├── videothread.* # 视频线程
├── audioplayer.* # 音频播放抽象 + Qt实现
├── videowidget.* # 视频渲染控件
├── myslider.* # 自定义进度条
```

---

## 技术架构
```text
            ┌──────────────┐
            │  DemuxThread │
            └──────┬───────┘
                   │
    ┌──────────────┴──────────────┐
    │                             │
   ┌──────────────┐ ┌──────────────┐
   │ VideoThread  │ │ AudioThread  │
   └──────┬───────┘ └──────┬───────┘
          │                │
          ▼                ▼
        VideoWidget AudioPlayer(Qt)
          │                │
          └───同步（PTS）───┘
```
### 架构说明

- DemuxThread 作为生产者，负责读取媒体流并分发数据
- AudioThread / VideoThread 作为消费者，独立解码处理
- 通过队列实现线程间解耦，避免阻塞
- 使用音频时钟统一系统时间基准

---

## 环境依赖

### 必须

- Qt 6（Core / Gui / Widgets / Multimedia）
- FFmpeg（建议 8.1 或以上）
### 示例路径（Windows）

```text
C:/Program Files/ffmpeg/ffmpeg8.1
```

---

## 构建方法

### 1. 修改 FFmpeg 路径

在 `CMakeLists.txt` 中：
```cmake
set(FFMPEG_PATH "你的FFmpeg路径")
```

### 2. 构建项目

```Bash (Debug)
mkdir build
cd build
cmake ..
cmake --build .
```

```Bash (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 3.运行

```Bash
./QtPlayer
```

---

## 使用说明
1. 点击 打开文件
2. 选择本地视频
3. 点击 播放 / 暂停
4. 拖动进度条进行 Seek
5. 双击窗口切换全屏

---

## 核心模块说明

### 1.DemuxThread（解封装线程）

- 打开媒体文件
- 读取 AVPacket
- 分发给音频/视频线程
- 实现 Seek
  
### 2.DecodeThread （解码基类）

- AVPacket 队列管理
- send / recv 解码流程
- 多线程安全
  
### 3.AudioThread（音频线程继承解码基类）

- 音频解码
- 重采样（SwrContext）
- 音频播放
- 提供 音频时钟（PTS）
  
### 4.VideoThread（视频线程解码基类）

- 视频解码
- 控制帧率
- 根据音频 PTS 同步
  
### 5.AudioPlayer

- 单一模式的音频播放接口
- Qt6 实现：QAudioSink
  
### 6.VideoWidget

- 使用 QImage + sws_scale
- 渲染视频帧

---

## 音视频同步原理

#### 核心思想
通过控制视频播放节奏，使其跟随音频时钟：

- 视频“慢了” → 丢帧追赶
- 视频“快了” → 延迟播放

---

#### 具体策略

- diff > 40：统一延迟 40ms 播放，优先保证流畅性
- -40 ≤ diff ≤ 40：按帧间隔播放（m_frame_duration_ms）
- diff < -40：丢弃当前帧追赶音频
- diff ≤ -100：丢弃当前 AVPacket 内所有帧（Seek/循环场景）

---

#### 特殊处理

- 使用 `lastSome` 原子变量处理视频尾帧，避免最后阶段同步失效

---

## 常见问题

### 1.播放失败

- 检查 FFmpeg 路径是否正确
- 下载 FFmpeg 版本是否正确，目录必须包含 include lib 等文件夹
- 检查 Qt 版本是否为 6.x
  
### 2. 没声音

- 检查系统默认音频设备
- 确认音频格式支持（48000 / 16bit / stereo）
  
### 3. 卡顿

- Debug 模式性能较差，建议 Release 编译
  
### 4.Release得到的.exe无法在文件夹中使用

- 打开终端（Qt 6.x MinGW / MSVC  或 VS 开发环境）并进入当前文件夹
```Bash
windeployqt QtPlayer.exe
```
- 若还是失败，将ffmpeg/bin内的以下文件拷贝进当前文件夹
```text
avcodec-xx.dll
avformat-xx.dll
avutil-xx.dll
swresample-xx.dll
swscale-xx.dll
```
---

## 未来优化方向

- 引入 **硬件解码（DXVA / VAAPI）** 提升性能
- 使用 **OpenGL 渲染** 替代 CPU 渲染，降低开销
- 实现 **自适应缓冲策略**，优化网络流播放体验
- 增加 **音视频同步策略优化（动态阈值）**
- 支持字幕系统（ASS / SRT）并实现时间轴同步

---

## 设计思考

### 为什么使用多线程？

- 解耦 IO、解码和渲染
- 提高资源利用率
- 避免单线程阻塞导致卡顿

---

### 为什么使用音频作为时钟？

- 音频对连续性要求更高
- 人耳对音频卡顿更敏感
- 视频更适合做动态调整（延迟 / 丢帧）

---

### 为什么使用队列？

- 实现生产者-消费者模型
- 平滑数据流，缓冲瞬时波动

## 开发难点与解决方案

### 1. 多线程死锁问题（AB-BA死锁）
- **问题**：线程间锁顺序不一致导致死锁
- **解决**：统一加锁顺序 + 日志定位 + 缩小锁粒度

---

### 2. Seek 时 UI 卡死
- **问题**：Seek 操作阻塞主线程
- **解决**：将 Seek 逻辑放入 DemuxThread 的 run 循环中异步执行

---

### 3. 暂停后音频仍播放
- **问题**：音频缓冲区未清空
- **解决**：
  - 清空缓冲区
  - 调用 `QIODevice::reset()`
  - 使用 `atomic<bool>` 控制暂停状态

---

### 4. Seek 过程中点击按钮崩溃
- **问题**：线程状态不一致导致非法访问
- **解决**：
  - Seek 期间禁用 UI 按钮
  - 使用信号槽控制状态

---

### 5. 视频播放卡顿 & 帧率异常

- **问题**：音视频同步策略不稳定，导致帧率波动与卡顿
- **解决**：
  - 重新设计同步策略（基于 diff 分段控制）
  - 引入帧间隔（m_frame_duration_ms）保证基础帧率
  - 通过日志分析与多场景测试（高帧率/低帧率/Seek）不断调整阈值

---

### 6. Seek 时等待时间异常

- **问题**：
  `av_seek_frame()` 使用的时间基（time_base）与播放器内部时间单位不一致，
  导致 Seek 后解码起点偏移，表现为跳转距离越大等待时间越长。

- **解决**：
  - 统一时间基：将输入时间从 UI 层的毫秒转换为流对应的 `time_base`（通过 `AVStream->time_base`）
  - 优化 Seek 后定位策略：
    - 仅定位关键帧（Key Frame）
    - 跳过 B 帧，减少解码开销
  - 实现快速恢复播放，降低 Seek 延迟

---

### 7. 视频播放末尾画面异常

- **问题**：
  DemuxThread 在发送完所有 AVPacket 后立即执行 `seek(0.0)`，
  未等待视频线程完成剩余帧渲染，导致播放尾部出现画面异常或跳变。

- **解决**：
  - 引入播放结束同步机制：
    - 通过 Qt 信号槽通知 VideoThread 进入“尾帧处理阶段”
  - 在尾帧阶段调整同步策略：
    - 暂停基于 `diff` 的同步控制
    - 按原始帧顺序完成剩余帧渲染
  - 视频线程播放完成后通知 DemuxThread 再执行循环播放
