# Core 迁出 / SDK 化：分叉说明

> 2026-11-19 的决定：**本仓库（QtPlayer）今后只做「Qt 播放器 + 推流器」，只修 bug**；
> `core/` 的 SDK 化在**新仓库**里做，做完后本仓库改为消费那个 SDK。

## 1. 分叉点

| 项 | 值 |
|---|---|
| 提交 | `028c226` + 未提交的「`op_core` 库目标」改动 |
| tag | `git tag core-fork-v1 && git push origin core-fork-v1`（提交之后打） |
| 备份 | `D:\桌面\QtPlayer-仓库备份-2026-11-19\`（完整历史 bundle + 源码 zip + 恢复说明） |

## 2. 本仓库今后的规矩

1. **只修 bug**，改 `app/` / `platform/` / `pusher/`；`core/` 尽量不动。
2. 必须动 `core/` 时，先分类：
   - **影响内核行为的**（状态机、线程、seek、像素格式、时钟）→ 先去 **SDK 仓库**修，
     再把提交 **回落** 到本仓库（见第 6 节）；
   - **只影响 Qt 侧表现的**（widget、事件、控件）→ 直接在本仓库改。
3. **不往 `core/` 加新对外接口**。新接口一律进 SDK 仓库的 `include/op/`。
4. 每次回落之后必须：三个目标都能构建 + `test_framequeue` 通过（CI 也会跑）。

## 3. 已经做好的准备（本次）

- `core/**` 编成独立静态库 **`op_core`**，不再是"源文件直接塞进 exe"：
  - `AUTOMOC / AUTOUIC / AUTORCC` 全部关闭；
  - 编译参数里**没有任何 Qt 路径**（只有源码根 + FFmpeg 头），这是"core 零 Qt"的**客观证据**，
    而不是靠 grep；
  - 万一以后有人往 `core/` 里加了 `Q_OBJECT`，会**直接编不过**，而不是被 moc 悄悄处理掉、
    然后在 Linux / NDK 上炸。
- `QtPlayer` 和 `test_framequeue` 都链 `op_core` —— **单测测的就是产品在用的同一份代码**，
  不再单独编一份 `framequeue.cpp`（那样测的和产品会慢慢漂开）。
- 删掉了 `playercore.h` 里两个"只有声明、没有实现"的接口
  （`setAudioPlayCallBack` / `setVideoPlayCallBack`），谁调谁链接失败，属于历史遗留。

## 4. 迁出时带走什么

| 目录 / 文件 | 处理 |
|---|---|
| `Player/QtPlayer/core/**` | **原样带走**（零 Qt、零平台分支，已核实） |
| `Player/QtPlayer/tests/test_framequeue.cpp` | 带走（本来就是 headless，不建窗口） |
| `Player/QtPlayer/cmake/FFmpeg.cmake` | 参考后重写（现在这份是给 QtPlayer 用的） |
| `Player/QtPlayer/platform/**`、`app/**` | **不带走**（Qt 壳，留在本仓库） |
| `docs/problems-and-solutions.md` | 建议复制一份带走（24~33 条全是 core 的坑） |

## 5. 新仓库要补的（本仓库不做）

1. **唯一的公开头** `include/op/op_player.h`（草案已放在本仓库 `include/op/op_player.h`）；
2. 门面实现（pimpl 包住 `PlayerCore` + `PlayerContext`，把内部全藏起来）；
3. **音频出口从 push 改成 pull**：`op_audio_read(buf, cap)`。
   桌面的 `IAudioOutput` 继续存在（Qt 播放器用），但 C 接口必须能"被拉" ——
   鸿蒙 `AudioRenderer` 和安卓 `AudioTrack` 都是"设备来要数据"；
4. **`op_open` 支持文件描述符**：鸿蒙 picker 给 URI、安卓给 `content://`，NDK 侧拿到的是 fd。
   只有路径参数的话，两个平台都要在外面"先拷进沙箱"；
5. 打包：CMake `install(TARGETS op_core ...)` + 版本号 + 许可说明（FFmpeg 是 LGPL）。

## 6. 回落（backport）流程

```
SDK 仓库：修 core 的 bug → git commit → 记下 hash
本仓库：  git cherry-pick <hash>
          冲突只在"本仓库 core 已被改动过"时才出现
          → 构建三目标 + 跑 test_framequeue → commit "sync core: <hash>"
```

每次回落都在 commit message 里写清 SDK 侧的 hash，方便回查。

## 7. 什么时候才改成 submodule

等 SDK 的 C 接口**稳定**（`op_player.h` 不再改）之后，再把本仓库切成
"submodule / `add_subdirectory` 消费 SDK、删掉 `core/`"。
在那之前，「一份拷贝 + 手动回落 + 版本标注」比 submodule 省事得多 ——
submodule 会让 Qt 这边每次拉代码都要多一步，还会把 CI 搞复杂。

## 8. 版本对应

| 日期 | 本仓库 | core 侧 |
|---|---|---|
| 2026-11-19 | `028c226` + `op_core` | 分叉点 `core-fork-v1`（首个 SDK 仓库的起点） |
