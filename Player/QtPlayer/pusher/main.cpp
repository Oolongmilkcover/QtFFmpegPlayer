/*
 * op_pusher —— 命令行版 mini-OBS（推流端）
 *
 * 链路：
 *   摄像头/虚拟摄像头 + 麦克风 → 采集 → 编码(H.264 + AAC) → 封装(mp4/FLV) → 文件 / RTMP → SRS
 *   （播放器是另一个独立进程，两端只通过 rtmp:// 这个 URL 相连）
 *
 * 里程碑（都已完成）：
 *   [0] --list-devices 列设备（先看清系统里有什么、名字怎么拼）
 *   [1] 采集 + 观测（打印实际参数 / --dump-frame 落一帧看图）
 *   [2] 编码（x264 → mp4，先用 ffprobe 对照 ffmpeg CLI 的产物）
 *   [3] 封装 + RTMP 推给 SRS + 断线重连
 *   [4] 音频（麦克风或合成正弦 → AAC → 同一个容器）
 *   [5] 参数实验开关（gop / B 帧 / zerolatency / CBR）+ --stats-csv 数据采集
 *
 * 常用命令：
 *   op_pusher --list-devices
 *   op_pusher --selftest                                   # 不需要任何硬件，全链路自检
 *   op_pusher --testsrc --duration 5 --dump-frame a.ppm    # 只看采集画面
 *   op_pusher --video "video=AMD Privacy View camera" --output demo.mp4 --duration 10
 *   op_pusher --testsrc --audio-input-format sine --audio "(合成)" --output av.mp4 --duration 5
 *   op_pusher --video "video=OBS Virtual Camera" --audio "audio=麦克风" \
 *             --url rtmp://<SRS 地址>/live/demo --stats-csv run.csv      # 推流 + 记数据
 */
#include "pusher.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include <string>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX                 // 不加这个，windows.h 的 min/max 宏会和 std 打架
#  include <windows.h>
#else
#  include <sys/resource.h>
#endif

static std::string toStd(const QString& s) { return s.toStdString(); }

// 按 url/文件名猜容器：rtmp/rtsp → flv；.mp4 → mp4；其余交给 FFmpeg 自己猜
static std::string guessFormat(const std::string& url)
{
    if (url.rfind("rtmp", 0) == 0 || url.rfind("rtsp", 0) == 0)
        return "flv";
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".mp4") == 0)
        return "mp4";
    return {};
}

/*
 * 进程累计消耗的 CPU 时间（秒）。
 *
 * 为什么自己做而不是看任务管理器：[第5步] 的参数实验要的是"可对照的数据"，
 * 把 CPU% 和码率/丢帧写进同一张 CSV，才能直接画图、直接写进报告。
 * 两次采样相减再除以墙钟间隔，就是这段时间的 CPU 占用率。
 */
static double processCpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME createTime, exitTime, kernelTime, userTime;
    if (!GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime))
        return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernelTime.dwLowDateTime;  k.HighPart = kernelTime.dwHighDateTime;
    u.LowPart = userTime.dwLowDateTime;    u.HighPart = userTime.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 1e7;   // FILETIME 单位是 100ns
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    return (ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6)
         + (ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
#endif
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("op_pusher");
    QCoreApplication::setApplicationVersion("0.2");

    // ---------------------------------------------------------------- 平台默认值
    // Windows：dshow，默认设备给 OBS 的虚拟摄像头（没有真摄像头时最省事）
    // Linux  ：v4l2 抓 /dev/video0，alsa 抓默认声卡
#ifdef Q_OS_WIN
    const QString defVideoDevice = "video=OBS Virtual Camera";
    const QString defVideoFormat = "dshow";
    const QString defAudioFormat = "dshow";
#else
    const QString defVideoDevice = "/dev/video0";
    const QString defVideoFormat = "v4l2";
    const QString defAudioFormat = "alsa";
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "mini-OBS：采集 → 编码(H.264/AAC) → 封装 → 落盘 mp4 或 RTMP 推流。\n"
        "没有摄像头/麦克风也能跑：--testsrc 合成视频、--audio-input-format sine 合成音频、--selftest 全链路自检。");
    parser.addHelpOption();
    parser.addVersionOption();

    // 注意：短选项不能和 QCommandLineParser 自带的撞车（-h 是 help、-v 是 version）
    QCommandLineOption optVideo(QStringList{ "video" },
        "视频设备。Windows(dshow) 形如 video=设备名；Linux(v4l2) 形如 /dev/video0", "device", defVideoDevice);
    QCommandLineOption optAudio(QStringList{ "a", "audio" },
        "音频设备（不写则只推视频）。Windows 形如 audio=麦克风；Linux 形如 default 或 hw:0", "device", "");
    QCommandLineOption optInputFormat(QStringList{ "f", "input-format" },
        "视频采集后端：dshow / v4l2 / testsrc(内置合成视频)", "format", defVideoFormat);
    QCommandLineOption optAudioInputFormat(QStringList{ "audio-input-format" },
        "音频采集后端：dshow / alsa / sine(内置 440Hz 合成音频)", "format", defAudioFormat);
    QCommandLineOption optSize(QStringList{ "size" },
        "分辨率提议 WxH（设备可能不采纳，一切以日志里的“实际”值为准）", "WxH", "1280x720");
    QCommandLineOption optFps(QStringList{ "fps" }, "帧率提议", "n", "30");
    QCommandLineOption optSampleRate(QStringList{ "sample-rate" }, "音频采样率提议", "Hz", "48000");
    QCommandLineOption optChannels(QStringList{ "channels" }, "音频声道数提议", "n", "2");
    QCommandLineOption optBitrate(QStringList{ "bitrate" }, "视频码率（bps）", "bps", "4000000");
    QCommandLineOption optAudioBitrate(QStringList{ "audio-bitrate" }, "音频码率（bps）", "bps", "128000");
    QCommandLineOption optPreset(QStringList{ "preset" }, "x264 preset", "name", "veryfast");
    QCommandLineOption optGop(QStringList{ "gop" }, "GOP 长度（帧）", "n", "30");
    QCommandLineOption optBframes(QStringList{ "max-bframes" }, "B 帧数量（推流一般 0）", "n", "0");
    QCommandLineOption optNoZeroLatency(QStringList{ "no-zerolatency" }, "关闭 tune=zerolatency（延迟对照实验用）");
    QCommandLineOption optCbr(QStringList{ "cbr" }, "钉住码率（接近 CBR，码率稳定性实验用）");
    QCommandLineOption optUrl(QStringList{ "url" }, "推流地址，例如 rtmp://192.168.1.50/live/demo", "url", "");
    QCommandLineOption optOutput(QStringList{ "o", "output" }, "落盘文件，例如 out.mp4", "file", "");
    QCommandLineOption optDuration(QStringList{ "duration" }, "运行时长（秒），0 表示一直跑到 Ctrl+C", "sec", "0");
    QCommandLineOption optTestsrc(QStringList{ "testsrc" }, "用内置合成视频源（不需要摄像头）");
    QCommandLineOption optDumpFrame(QStringList{ "dump-frame" },
        "把采集到的第一帧存成 PPM 图片（没有预览窗口时用它肉眼验收）", "file.ppm", "");
    QCommandLineOption optListDevices(QStringList{ "list-devices" }, "列出采集设备后退出");
    QCommandLineOption optSelfTest(QStringList{ "selftest" },
        "自检：合成视频+合成音频 → 编码 → 落盘 mp4 → 落一帧 PPM，跑 3 秒后退出（不需要任何硬件）");
    QCommandLineOption optStatsInterval(QStringList{ "stats-interval" }, "统计打印间隔（秒）", "sec", "1");
    QCommandLineOption optStatsCsv(QStringList{ "stats-csv" },
        "把每秒统计写成 CSV（做参数实验/画图用）", "file.csv", "");
    QCommandLineOption optReconnectAttempts(QStringList{ "reconnect-attempts" },
        "推流断开后的最大重连次数，0=一直重试", "n", "0");
    QCommandLineOption optReconnectDelay(QStringList{ "reconnect-delay" },
        "首次重连等待（毫秒，之后指数退避，上限 15 秒）", "ms", "1000");

    parser.addOptions({ optVideo, optAudio, optInputFormat, optAudioInputFormat, optSize, optFps,
                        optSampleRate, optChannels, optBitrate, optAudioBitrate, optPreset, optGop,
                        optBframes, optNoZeroLatency, optCbr, optUrl, optOutput, optDuration,
                        optTestsrc, optDumpFrame, optListDevices, optSelfTest,
                        optStatsInterval, optStatsCsv, optReconnectAttempts, optReconnectDelay });
    parser.process(app);

    const bool useSynthetic = parser.isSet(optTestsrc) || parser.isSet(optSelfTest);

    // ---------------------------------------------------------------- [第0步] 列设备
    if (parser.isSet(optListDevices)) {
        const std::string vf = toStd(parser.value(optInputFormat));
        qInfo() << "== 列出设备（后端" << vf.c_str() << "）==";
        listInputDevices(vf);
#ifndef Q_OS_WIN
        // Windows 的 dshow 一次把音视频都列出来；Linux 是 v4l2 / alsa 两套后端，要分别列
        const std::string af = toStd(parser.value(optAudioInputFormat));
        qInfo() << "== 列出音频设备（后端" << af.c_str() << "）==";
        listInputDevices(af);
#endif
        qInfo() << "提示：dshow 的设备名要写成  video=名字  /  audio=名字（上面已按类型标好前缀）";
        return 0;
    }

    // ---------------------------------------------------------------- 组装配置
    PusherConfig cfg;
    cfg.video.device      = toStd(parser.value(optVideo));
    cfg.video.inputFormat = toStd(parser.value(optInputFormat));
    cfg.audio.device      = toStd(parser.value(optAudio));
    cfg.audio.inputFormat = toStd(parser.value(optAudioInputFormat));

    const QStringList wh = parser.value(optSize).split('x');
    if (wh.size() == 2) {
        cfg.video.width  = wh.at(0).toInt();
        cfg.video.height = wh.at(1).toInt();
    }
    cfg.video.fps          = parser.value(optFps).toInt();
    cfg.audio.sampleRate   = parser.value(optSampleRate).toInt();
    cfg.audio.channels     = parser.value(optChannels).toInt();
    cfg.withAudio          = !cfg.audio.device.empty()
                             || cfg.audio.inputFormat == "sine";   // 合成音频不需要设备名，给了格式就启用

    cfg.encoder.gopSize      = parser.value(optGop).toInt();
    cfg.encoder.maxBFrames   = parser.value(optBframes).toInt();
    cfg.encoder.preset       = toStd(parser.value(optPreset));
    cfg.encoder.videoBitrate = parser.value(optBitrate).toInt();
    cfg.encoder.audioBitrate = parser.value(optAudioBitrate).toInt();
    cfg.encoder.zerolatency  = !parser.isSet(optNoZeroLatency);
    cfg.encoder.cbr          = parser.isSet(optCbr);

    cfg.output.url    = toStd(parser.value(optUrl).isEmpty() ? parser.value(optOutput)
                                                             : parser.value(optUrl));
    cfg.output.format = guessFormat(cfg.output.url);

    cfg.durationSeconds       = parser.value(optDuration).toInt();
    cfg.dumpFirstFrame        = toStd(parser.value(optDumpFrame));
    cfg.reconnectMaxAttempts  = parser.value(optReconnectAttempts).toInt();
    cfg.reconnectDelayMs      = parser.value(optReconnectDelay).toInt();

    // 合成源：--testsrc 和 --selftest 都要把输入格式切过去
    // （曾经漏过这一步：算了 useSynthetic 却没用，导致 --testsrc 悄悄去开了真实设备）
    if (useSynthetic) {
        cfg.video.inputFormat = "testsrc";
        cfg.video.device      = "(内置合成视频)";
    }

    // 自检模式：合成视频 + 合成音频 + 落盘 mp4 + 落一帧 PPM，一条命令验完整链路
    // 注意：--selftest 打开音频时，音频设备名写什么都行（合成源不看名字），这里给个可读的
    if (parser.isSet(optSelfTest)) {
        cfg.audio.inputFormat = "sine";
        cfg.audio.device      = "(内置合成音频)";
        cfg.withAudio         = true;
        if (cfg.durationSeconds == 0) cfg.durationSeconds = 3;
        if (cfg.dumpFirstFrame.empty()) cfg.dumpFirstFrame = "op_pusher_frame.ppm";
        if (cfg.output.url.empty()) cfg.output.url = "op_pusher_selftest.mp4";
        if (cfg.output.format.empty()) cfg.output.format = "mp4";
    }

    const bool wantCsv = !parser.value(optStatsCsv).isEmpty();
    const QString csvPath = parser.value(optStatsCsv);
    const int statsIntervalSec = parser.value(optStatsInterval).toInt() > 0
                                     ? parser.value(optStatsInterval).toInt() : 1;
    const int durationSeconds = cfg.durationSeconds;
    const bool isSelfTest = parser.isSet(optSelfTest);

    qInfo().nospace() << "配置: 视频源=" << cfg.video.device.c_str()
                      << " (" << cfg.video.inputFormat.c_str() << ")"
                      << "  提议 " << cfg.video.width << "x" << cfg.video.height << "@" << cfg.video.fps
                      << (cfg.withAudio ? ("  音频源=" + cfg.audio.device).c_str() : "  （只采集视频）");
    qInfo().nospace() << "编码: " << cfg.encoder.preset.c_str()
                      << " gop=" << cfg.encoder.gopSize
                      << " bframes=" << cfg.encoder.maxBFrames
                      << " 码率=" << cfg.encoder.videoBitrate
                      << (cfg.encoder.zerolatency ? " zerolatency" : "")
                      << (cfg.encoder.cbr ? " CBR" : "")
                      << "  输出=" << (cfg.output.url.empty() ? "(只采集)" : cfg.output.url.c_str());
    if (cfg.output.url.empty())
        qInfo() << "未指定 --url/--output：本次只做采集观测";

    // ---------------------------------------------------------------- 启动
    Pusher pusher(std::move(cfg));
    if (!pusher.start()) {
        qCritical() << "启动失败";
        return 1;
    }

    qInfo().nospace() << "开始: " << pusher.actualWidth() << "x" << pusher.actualHeight()
                      << "@" << pusher.actualFps()
                      << (pusher.actualSampleRate() > 0
                              ? ("  音频 " + std::to_string(pusher.actualSampleRate()) + "Hz "
                                 + std::to_string(pusher.actualChannels()) + "声道").c_str()
                              : "");

    // ---------------------------------------------------------------- 统计（打印 + CSV）
    QFile csvFile;
    QTextStream csvOut(&csvFile);
    if (wantCsv) {
        csvFile.setFileName(csvPath);
        if (csvFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            // 列的顺序和 Pusher::statsCsvRow() 完全对应
            csvOut << "elapsed_s,videoFrames,audioFrames,videoEncoded,audioEncoded,packetsWritten,"
                      "packetsDropped,reconnects,queueFrames,queuePackets,bytes,kbps,cpuPercent\n";
            csvOut.flush();
            qInfo() << "[stats] CSV 写到" << csvPath;
        } else {
            qWarning() << "[stats] 打不开 CSV 文件:" << csvPath;
        }
    }

    QElapsedTimer wall;
    wall.start();
    double lastCpuSeconds = processCpuSeconds();
    qint64 lastSampleMs = 0;

    QTimer statsTimer;
    QObject::connect(&statsTimer, &QTimer::timeout, [&] {
        const qint64 nowMs = wall.elapsed();
        const double dtSec = (nowMs - lastSampleMs) / 1000.0;
        const double cpuSeconds = processCpuSeconds();
        // CPU% = 这段时间消耗的 CPU 时间 / 墙钟时间（多线程可以超过 100%）
        const double cpuPercent = dtSec > 0.01 ? ((cpuSeconds - lastCpuSeconds) / dtSec) * 100.0 : 0.0;
        lastCpuSeconds = cpuSeconds;
        lastSampleMs = nowMs;

        pusher.dumpStatsLine();
        if (csvFile.isOpen()) {
            csvOut << QString::fromStdString(pusher.statsCsvRow(nowMs / 1000.0, cpuPercent)) << "\n";
            csvOut.flush();     // 边跑边刷，随时可以打开看
        }
    });
    statsTimer.start(statsIntervalSec * 1000);

    if (durationSeconds > 0) {
        QTimer::singleShot(durationSeconds * 1000, &app, [&pusher] {
            qInfo() << "到达 --duration，准备停止（stop 里会先打断阻塞读，再把剩余数据写完）";
            pusher.stop();
            QCoreApplication::quit();
        });
    } else {
        qInfo() << "按 Ctrl+C 结束（终端里 Ctrl+C 会让进程直接退出，"
                   "要优雅退出请用 --duration 或从任务管理器发关闭信号）";
    }

    const int rc = app.exec();
    pusher.stop();   // 幂等：duration 那条路已经停过了，这里再调一次没副作用

    if (csvFile.isOpen()) {
        csvFile.close();
        qInfo() << "[stats] CSV 已收尾:" << csvPath;
    }

    // ---------------------------------------------------------------- 自检结论
    if (isSelfTest) {
        const auto& st = pusher.stats();
        const bool ok = st.encoded.load() > 0 && st.sent.load() > 0;
        qInfo().nospace() << (ok ? "[selftest] 通过" : "[selftest] 失败")
                          << ": 采集视频=" << st.videoFrames.load()
                          << " 采集音频=" << st.audioFrames.load()
                          << " 视频编码=" << st.encoded.load()
                          << " 音频编码=" << st.audioEncoded.load()
                          << " 写出=" << st.sent.load() << " 个包"
                          << " 丢帧=" << 0 << "（详见上面的 [stats] 行）";
        if (!ok) return 1;
    }

    return rc;
}
