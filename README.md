# 基于 Qt 6.11 + FFmpeg 8.1 的多线程音视频播放器（支持音画同步 / Seek / 实时播放）

一个基于 **Qt6.11 + FFmpeg8.1** 实现的简易多线程音视频播放器，支持音视频同步、拖动进度、暂停播放等功能。

---

## 目录

- [项目简介](#项目简介)
- [项目特点](#项目特点)
- [功能特性](#功能特性)
- [项目结构](#项目结构)
- [技术架构](#技术架构)
- [环境依赖](#环境依赖)
- [构建方法](#构建方法)
- [使用说明](#使用说明)
- [核心模块说明](#核心模块说明)
- [音视频同步原理](#音视频同步原理)
- [未来优化](#未来优化)
- [总结](#总结)

---

## 项目简介

QtPlayer 是一个基于 Qt6 和 FFmpeg8.1 开发的桌面视频播放器，实现了：

- 视频解封装（Demux）
- 音视频解码（Decode）
- 音频播放（QAudioSink）
- 视频渲染（QWidget）
- 音视频同步（Audio Clock）

适合作为 **FFmpeg + Qt 多线程播放器学习项目**。

---

## 项目特点
- 基于 **Qt 6.11 + FFmpeg 8.1**，兼容最新 API（非过时实现）
- 实现完整播放器链路：**Demux → Decode → Render**
- 多线程解耦架构（解封装 / 音频 / 视频独立线程）
- 基于 **音频时钟（Audio Clock）实现音视频同步**
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
### 音频为主时钟
- 音频线程计算当前播放时间 pts
- 视频线程获取 synpts
- 比较差值：
```C++
diff = video_pts - audio_pts
```
### 同步策略：
- diff > 0 → 视频等待（延迟）最大等500ms避免卡死
- diff < -40ms → 丢帧
- 否则 → 正常播放

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
- 打开终端（Qt MinGW / MSVC  或 VS 开发环境）
```Bash
windeployqt QtPlayer.exe
```
- 将ffmpeg/bin内的一下文件拷贝进当前文件夹
```text
avcodec-xx.dll
avformat-xx.dll
avutil-xx.dll
swresample-xx.dll
swscale-xx.dll
```
---

## 未来优化
- 支持硬件解码（DXVA / VAAPI）
- 使用 OpenGL 渲染（替代 QImage 提升性能）
- 播放列表管理
- 字幕（ASS / SRT）支持
- 音量 UI 控制
- 网络流优化（RTSP 低延迟 / 缓冲控制）

---

## 总结
网上搜到的教程都太老了，所以作者用Qt 6.11 + FFmpeg 8.1写了个最新版本的播放器demo可供学习与参考。

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