/*
 * op_pusher —— 命令行版 mini-OBS（推流端），不含任何 Qt 依赖。
 *
 * 链路：摄像头/虚拟摄像头 + 麦克风 → 采集 → 编码(H.264 + AAC) → 封装(mp4/FLV) → 文件 / RTMP
 *
 * 里程碑（全部完成）：
 *   [0] --list-devices 列设备   [1] 采集 + 观测（--dump-frame 落一帧看图）
 *   [2] 编码落盘 mp4           [3] RTMP 推流 + 断线重连
 *   [4] 音频（麦克风或合成正弦 → AAC）
 *   [5] 参数开关（gop / B 帧 / zerolatency / CBR）+ --stats-csv 数据采集
 *
 * 常用：
 *   op_pusher --list-devices
 *   op_pusher --selftest                                   # 不需要任何硬件，全链路自检
 *   op_pusher --testsrc --duration 5 --dump-frame a.ppm
 *   op_pusher --video "video=AMD Privacy View camera" --output demo.mp4 --duration 10
 *   op_pusher --testsrc --audio-input-format sine --output av.mp4 --duration 5
 *   op_pusher --url rtmp://<SRS>/live/demo --stats-csv run.csv
 *
 * 退出：Ctrl+C 会走优雅停止（先打断阻塞读 → flush 编码器 → 写 trailer）。
 */
#include "pusher.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX                 // 不加这个，windows.h 的 min/max 宏会和 std 打架
#  include <windows.h>
#  include <shellapi.h>          // CommandLineToArgvW：命令行按 UTF-16 取，再显式转 UTF-8
#else
#  include <csignal>
#  include <sys/resource.h>
#endif

namespace {

std::atomic<bool> g_stopRequested{ false };

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD)
{
    g_stopRequested.store(true);
    return TRUE;      // 告诉系统"我处理了"，别直接杀进程
}
#else
void sigintHandler(int) { g_stopRequested.store(true); }
#endif

// 进程累计 CPU 时间（秒）。两次采样相减 / 墙钟间隔 = CPU 占用率。
double processCpuSeconds()
{
#ifdef _WIN32
    FILETIME ct, et, kt, ut;
    if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kt.dwLowDateTime;  k.HighPart = kt.dwHighDateTime;
    u.LowPart = ut.dwLowDateTime;  u.HighPart = ut.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 1e7;   // FILETIME 单位 100ns
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    return (ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6)
         + (ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
#endif
}

// 按 url/文件名猜容器：rtmp/rtsp → flv；.mp4 → mp4；其余交给 FFmpeg 自己猜
std::string guessFormat(const std::string& url)
{
    if (url.rfind("rtmp", 0) == 0 || url.rfind("rtsp", 0) == 0) return "flv";
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".mp4") == 0) return "mp4";
    return {};
}

struct Options
{
    std::string videoDevice, videoFormat;
    std::string audioDevice, audioFormat;
    int width = 1280, height = 720, fps = 30;
    int sampleRate = 48000, channels = 2;
    int videoBitrate = 4000000, audioBitrate = 128000;
    int gop = 30, maxBFrames = 0;
    std::string preset = "veryfast";
    bool zerolatency = true, cbr = false;
    std::string url, output, dumpFrame, statsCsv;
    int duration = 0, statsInterval = 1;
    int reconnectAttempts = 0, reconnectDelay = 1000;
    bool testsrc = false, selftest = false, listDevices = false;
};

void printUsage()
{
    std::printf(
        "op_pusher —— 采集 → 编码(H.264/AAC) → 封装 → 落盘 mp4 或 RTMP 推流（无 Qt 依赖）\n"
        "\n"
        "  没有摄像头/麦克风也能跑：--testsrc 合成视频、--audio-input-format sine 合成音频、--selftest 全链路自检\n"
        "\n"
        "设备与来源：\n"
        "  --video <dev>              视频设备（dshow: video=名字；v4l2: /dev/video0）\n"
        "  -a, --audio <dev>          音频设备（不给则只采集视频）\n"
        "  -f, --input-format <fmt>   视频采集后端：dshow / v4l2 / testsrc\n"
        "  --audio-input-format <fmt> 音频采集后端：dshow / alsa / sine\n"
        "  --list-devices             列出采集设备后退出\n"
        "\n"
        "采集/编码参数：\n"
        "  --size WxH                 分辨率提议（设备可能不采纳，以日志里的实际值为准）\n"
        "  --fps <n>                  帧率提议            --sample-rate <Hz>  采样率提议\n"
        "  --channels <n>             声道数提议          --bitrate <bps>     视频码率\n"
        "  --audio-bitrate <bps>      音频码率            --preset <name>     x264 preset\n"
        "  --gop <n>                  关键帧间隔          --max-bframes <n>   B 帧数量\n"
        "  --no-zerolatency           关闭 tune=zerolatency\n"
        "  --cbr                      钉住码率（接近 CBR）\n"
        "\n"
        "输出：\n"
        "  --url <url>                推流地址，如 rtmp://127.0.0.1/live/demo\n"
        "  -o, --output <file>        落盘文件，如 out.mp4\n"
        "  --duration <sec>           运行时长，0 = 直到 Ctrl+C\n"
        "  --dump-frame <file.ppm>    把第一帧存成 PPM（没有预览窗口时肉眼验收）\n"
        "  --stats-interval <sec>     统计打印间隔         --stats-csv <file>  统计写 CSV\n"
        "  --reconnect-attempts <n>   推流断线重连上限，0 = 一直重试\n"
        "  --reconnect-delay <ms>     首次重连等待（之后指数退避，上限 15s）\n"
        "\n"
        "  --testsrc                  用内置合成视频源\n"
        "  --selftest                 全链路自检：合成视频+音频 → mp4 → 3 秒后退出\n"
        "  -h, --help                 显示本帮助\n");
}

// 取下一个参数（--key value 或 --key=value 都支持）；不是这个 key 就返回 false
bool takeValue(const std::vector<std::string>& a, size_t& i, const char* key, std::string& out)
{
    const std::string cur = a[i];
    const std::string k(key);
    if (cur.rfind(k + "=", 0) == 0) { out = cur.substr(k.size() + 1); return true; }
    if (cur == k) {
        if (i + 1 >= a.size()) { std::fprintf(stderr, "参数 %s 缺少取值\n", key); return false; }
        out = a[++i];
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    const char* defVideo    = "video=OBS Virtual Camera";
    const char* defVideoFmt = "dshow";
    const char* defAudioFmt = "dshow";
#else
    std::signal(SIGINT, sigintHandler);
    const char* defVideo    = "/dev/video0";
    const char* defVideoFmt = "v4l2";
    const char* defAudioFmt = "alsa";
#endif

    Options o;
    o.videoDevice = defVideo;
    o.videoFormat = defVideoFmt;
    o.audioFormat = defAudioFmt;

    // [重点] 命令行参数的编码问题：
    //   cmd 的代码页一般是 936(GBK)，argv 里的中文是"本地编码"字节；
    //   而 FFmpeg/dshow 要的是 UTF-8（比如设备名 "麦克风 (UGREEN CM564 USB Audio )"）。
    //   直接把 argv 交给 avformat_open_input 会把中文名字解成乱码，报 "Could not find ... device"。
    //   所以 Windows 下从 UTF-16 命令行重新取参数，逐条转成 UTF-8。
    std::vector<std::string> args;
#ifdef _WIN32
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv) {
            for (int k = 1; k < wargc; ++k) {
                const int need = WideCharToMultiByte(CP_UTF8, 0, wargv[k], -1, nullptr, 0, nullptr, nullptr);
                std::string s(need > 1 ? static_cast<size_t>(need - 1) : 0, '\0');
                if (need > 1)
                    WideCharToMultiByte(CP_UTF8, 0, wargv[k], -1, &s[0], need, nullptr, nullptr);
                args.push_back(std::move(s));
            }
            LocalFree(wargv);
        }
    }
#else
    (void)argc;
    args.assign(argv + 1, argv + argc);
#endif
    for (size_t i = 0; i < args.size(); ++i) {
        std::string v;
        if (args[i] == "-h" || args[i] == "--help") { printUsage(); return 0; }
        if (takeValue(args, i, "--video", v))              { o.videoDevice = v; continue; }
        if (takeValue(args, i, "-a", v))                   { o.audioDevice = v; continue; }
        if (takeValue(args, i, "--audio", v))              { o.audioDevice = v; continue; }
        if (takeValue(args, i, "-f", v))                   { o.videoFormat = v; continue; }
        if (takeValue(args, i, "--input-format", v))       { o.videoFormat = v; continue; }
        if (takeValue(args, i, "--audio-input-format", v)) { o.audioFormat = v; continue; }
        if (takeValue(args, i, "--size", v)) {
            const auto x = v.find('x');
            if (x != std::string::npos) {
                o.width  = std::atoi(v.substr(0, x).c_str());
                o.height = std::atoi(v.substr(x + 1).c_str());
            }
            continue;
        }
        if (takeValue(args, i, "--fps", v))                { o.fps = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--sample-rate", v))        { o.sampleRate = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--channels", v))           { o.channels = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--bitrate", v))            { o.videoBitrate = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--audio-bitrate", v))      { o.audioBitrate = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--preset", v))             { o.preset = v; continue; }
        if (takeValue(args, i, "--gop", v))                { o.gop = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--max-bframes", v))        { o.maxBFrames = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--url", v))                { o.url = v; continue; }
        if (takeValue(args, i, "-o", v))                   { o.output = v; continue; }
        if (takeValue(args, i, "--output", v))             { o.output = v; continue; }
        if (takeValue(args, i, "--duration", v))           { o.duration = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--dump-frame", v))         { o.dumpFrame = v; continue; }
        if (takeValue(args, i, "--stats-interval", v))     { o.statsInterval = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--stats-csv", v))          { o.statsCsv = v; continue; }
        if (takeValue(args, i, "--reconnect-attempts", v)) { o.reconnectAttempts = std::atoi(v.c_str()); continue; }
        if (takeValue(args, i, "--reconnect-delay", v))    { o.reconnectDelay = std::atoi(v.c_str()); continue; }
        if (args[i] == "--testsrc")        { o.testsrc = true; continue; }
        if (args[i] == "--selftest")       { o.selftest = true; continue; }
        if (args[i] == "--list-devices")   { o.listDevices = true; continue; }
        if (args[i] == "--no-zerolatency") { o.zerolatency = false; continue; }
        if (args[i] == "--cbr")            { o.cbr = true; continue; }
        std::fprintf(stderr, "未知参数: %s（--help 看用法）\n", args[i].c_str());
        return 2;
    }

    // ---------------------------------------------------------------- [第0步] 列设备
    if (o.listDevices) {
        std::printf("== 列出设备（后端 %s）==\n", o.videoFormat.c_str());
        listInputDevices(o.videoFormat);
#ifndef _WIN32
        std::printf("== 列出音频设备（后端 %s）==\n", o.audioFormat.c_str());
        listInputDevices(o.audioFormat);
#endif
        std::printf("提示：dshow 的设备名要写成  video=名字  /  audio=名字（上面已按类型标好前缀）\n");
        return 0;
    }

    // ---------------------------------------------------------------- 组装配置
    PusherConfig cfg;
    cfg.video.device      = o.videoDevice;
    cfg.video.inputFormat = o.videoFormat;
    cfg.video.width       = o.width;
    cfg.video.height      = o.height;
    cfg.video.fps         = o.fps;
    cfg.audio.device      = o.audioDevice;
    cfg.audio.inputFormat = o.audioFormat;
    cfg.audio.sampleRate  = o.sampleRate;
    cfg.audio.channels    = o.channels;
    cfg.withAudio         = !o.audioDevice.empty() || o.audioFormat == "sine";

    cfg.encoder.width        = o.width;
    cfg.encoder.height       = o.height;
    cfg.encoder.fps          = o.fps;
    cfg.encoder.videoBitrate = o.videoBitrate;
    cfg.encoder.audioBitrate = o.audioBitrate;
    cfg.encoder.gopSize      = o.gop;
    cfg.encoder.maxBFrames   = o.maxBFrames;
    cfg.encoder.preset       = o.preset;
    cfg.encoder.zerolatency  = o.zerolatency;
    cfg.encoder.cbr          = o.cbr;

    cfg.output.url    = o.url.empty() ? o.output : o.url;
    cfg.output.format = guessFormat(cfg.output.url);

    cfg.durationSeconds      = o.duration;
    cfg.dumpFirstFrame       = o.dumpFrame;
    cfg.reconnectMaxAttempts = o.reconnectAttempts;
    cfg.reconnectDelayMs     = o.reconnectDelay;

    // 合成源：--testsrc / --selftest 都把输入格式切过去
    if (o.testsrc || o.selftest) {
        cfg.video.inputFormat = "testsrc";
        cfg.video.device      = "(内置合成视频)";
    }
    // 自检：合成视频 + 合成音频 + 落盘 mp4 + 落一帧 PPM，3 秒
    if (o.selftest) {
        cfg.audio.inputFormat = "sine";
        cfg.audio.device      = "(内置合成音频)";
        cfg.withAudio         = true;
        if (cfg.durationSeconds == 0) cfg.durationSeconds = 3;
        if (cfg.dumpFirstFrame.empty()) cfg.dumpFirstFrame = "op_pusher_frame.ppm";
        if (cfg.output.url.empty())     cfg.output.url = "op_pusher_selftest.mp4";
        if (cfg.output.format.empty())  cfg.output.format = "mp4";
    }

    std::printf("配置: 视频源=%s (%s)  提议 %dx%d@%d%s\n",
                cfg.video.device.c_str(), cfg.video.inputFormat.c_str(),
                cfg.video.width, cfg.video.height, cfg.video.fps,
                cfg.withAudio ? ("  音频源=" + cfg.audio.device).c_str() : "  （只采集视频）");
    std::printf("编码: %s gop=%d bframes=%d 码率=%d%s%s  输出=%s\n",
                cfg.encoder.preset.c_str(), cfg.encoder.gopSize, cfg.encoder.maxBFrames,
                cfg.encoder.videoBitrate, cfg.encoder.zerolatency ? " zerolatency" : "",
                cfg.encoder.cbr ? " CBR" : "", cfg.output.url.empty() ? "(只采集)" : cfg.output.url.c_str());
    if (cfg.output.url.empty()) std::printf("未指定 --url/--output：本次只做采集观测\n");

    // ---------------------------------------------------------------- 启动
    const bool isSelfTest = o.selftest;
    const int  durationSeconds = cfg.durationSeconds;
    const int  statsInterval = o.statsInterval > 0 ? o.statsInterval : 1;
    const std::string csvPath = o.statsCsv;

    Pusher pusher(std::move(cfg));
    if (!pusher.start()) {
        std::fprintf(stderr, "启动失败\n");
        return 1;
    }
    std::printf("开始: %dx%d@%d", pusher.actualWidth(), pusher.actualHeight(), pusher.actualFps());
    if (pusher.actualSampleRate() > 0)
        std::printf("  音频 %dHz %d声道", pusher.actualSampleRate(), pusher.actualChannels());
    std::printf("\n");
    if (durationSeconds <= 0) std::printf("按 Ctrl+C 结束（会走优雅停止：flush 编码器 + 写 trailer）\n");

    // ---------------------------------------------------------------- 统计（打印 + CSV）
    FILE* csv = nullptr;
    if (!csvPath.empty()) {
        csv = std::fopen(csvPath.c_str(), "wb");
        if (csv) {
            std::fprintf(csv, "elapsed_s,videoFrames,audioFrames,videoEncoded,audioEncoded,"
                              "packetsWritten,packetsDropped,reconnects,queueFrames,queuePackets,"
                              "bytes,kbps,cpuPercent\n");
            std::fflush(csv);
            std::printf("[stats] CSV 写到 %s\n", csvPath.c_str());
        } else {
            std::fprintf(stderr, "[stats] 打不开 CSV 文件: %s\n", csvPath.c_str());
        }
    }

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    double lastCpu = processCpuSeconds();
    auto lastSample = t0;
    auto nextStats  = t0 + std::chrono::seconds(statsInterval);

    // 主循环：100ms 一跳，负责统计打印、CSV、到点退出、Ctrl+C 响应
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = Clock::now();

        if (now >= nextStats) {
            const double dtSec = std::chrono::duration<double>(now - lastSample).count();
            const double cpu = processCpuSeconds();
            const double cpuPct = dtSec > 0.01 ? ((cpu - lastCpu) / dtSec) * 100.0 : 0.0;
            lastCpu = cpu;
            lastSample = now;
            nextStats += std::chrono::seconds(statsInterval);

            pusher.dumpStatsLine();
            if (csv) {
                const double elapsed = std::chrono::duration<double>(now - t0).count();
                const std::string row = pusher.statsCsvRow(elapsed, cpuPct);
                std::fprintf(csv, "%s\n", row.c_str());
                std::fflush(csv);      // 边跑边刷，随时可以打开看
            }
        }

        if (g_stopRequested.load()) {
            std::printf("收到停止信号，优雅停止中（flush + trailer）...\n");
            break;
        }
        if (durationSeconds > 0 && now - t0 >= std::chrono::seconds(durationSeconds)) {
            std::printf("到达 --duration，准备停止\n");
            break;
        }
    }

    pusher.stop();

    if (csv) { std::fclose(csv); std::printf("[stats] CSV 已收尾: %s\n", csvPath.c_str()); }

    // ---------------------------------------------------------------- 自检结论
    if (isSelfTest) {
        const auto& st = pusher.stats();
        const bool ok = st.encoded.load() > 0 && st.sent.load() > 0;
        std::printf("[selftest] %s: 采集视频=%llu 采集音频=%llu 视频编码=%llu 音频编码=%llu 写出=%llu 个包\n",
                    ok ? "通过" : "失败",
                    static_cast<unsigned long long>(st.videoFrames.load()),
                    static_cast<unsigned long long>(st.audioFrames.load()),
                    static_cast<unsigned long long>(st.encoded.load()),
                    static_cast<unsigned long long>(st.audioEncoded.load()),
                    static_cast<unsigned long long>(st.sent.load()));
        if (!ok) return 1;
    }
    return 0;
}
