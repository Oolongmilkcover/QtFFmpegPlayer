#ifndef PLAYER_H
#define PLAYER_H
#include "core/demuxthread.h"
#include <QElapsedTimer>
#include <QListWidgetItem>
#include <QWidget>
#include "core/perfclock.h"
#include <QString>
class PlayerCore;
struct PlayerMessage;
class VideoWidget;
class AudioPlayer;

QT_BEGIN_NAMESPACE
namespace Ui {
class Player;

}
QT_END_NAMESPACE

class Player : public QWidget
{
    Q_OBJECT

public:
    Player(QWidget *parent = nullptr);
    ~Player();

protected:
    //自适应窗口
    void resizeEvent(QResizeEvent * e) override;
    //双击切换全屏
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    //进度条动态显示 定时器
    void timerEvent(QTimerEvent *e) override;
    //关闭
    void closeEvent(QCloseEvent* e) override;

    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    void keyPressEvent(QKeyEvent *e) override;

    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    //边缘缩放用
    Qt::Edges hitTestEdges(const QPoint &globalPos) const;
    void beginResize(Qt::Edges edges, const QPoint &globalPos);
    void updateResize(const QPoint &globalPos);
    static constexpr int kBorder = 6;      // 边缘热区宽度(px)
    bool       m_isResizing = false;       // 是否正在缩放
    Qt::Edges  m_resizeEdge = {};          // 当前缩放的边
    QPoint     m_pressGlobal = {};         // 按下时的全局鼠标位置
    QRect      m_startGeo = {};            // 开始缩放时的窗口几何
    QSize      m_minSize = {};             // 最小尺寸快照

    void addToPlayList(const QString &path);

    //退出逐帧模式（恢复按钮状态，让dt把播放位置对齐）
    void exitStepFrame();

signals:
    void setPausePicture(bool isPause);

    void setPlayingText(QString name);

    void maxOrRestore();
private slots:
    void play();

    void pause();
    //进度条
    void sliderSeek(double pos);

    void on_openFile();

    void on_openLink();

    void setVolume(double pos);

    //全屏
    void toggleFullScreen();

    //停止播放(不是暂停)
    void stopToPlay();

    //显示或隐藏列表
    void showOrHidePlayList();

    void playFile(const QString &path);


    void playNext();

    void playPrev();

    void onPlayList_doubleClicked(QListWidgetItem *item);

    void ffSeekFiveSec();

    void rewindSeekFiveSec();

    // 快进/快退统一入口：把步长累加到“待生效目标”并重启去抖定时器（deltaMs 可正可负）
    void queueSeekBy(long long deltaMs);

    void stepFrame(int mode);

    void adjustVolume(double delta);

    void changeSpeed(double delta);

    //读消息时调用的私有函数
    void onEVT_PREPARED(PlayerMessage& msg);

    void onEVT_POSITION(PlayerMessage& msg);

    void onDISABLEBTN();

    void onABLEBTN();

    void onEVT_ERROR(PlayerMessage& msg);

    void onSEEkDONE(PlayerMessage& msg);
private:
    Ui::Player *ui;

    PlayerContext* m_playerCtx;

    PlayerCore*  m_playerCore;

    void setEventCallback();

    VideoWidget* m_widget;
    void setVideoPainterCb();

    /*
     * seek 的两个私有出口/工具（所有 seek 请求都必须走 postSeek，别再各处自己 postMessage）：
     *   postSeek()     ：刷进度条 → 钉住目标 → 置 m_canSeek=false → 投递 CMD_SEEK；
     *   pinSeekTarget()：只钉住显示，不投递（去抖累积期间用）。
     * 这样"串行化"和"进度条钉住"只有一处实现，不会像之前那样尾部 seek 绕过 m_canSeek。
     */
    void postSeek(long long targetMs);
    void pinSeekTarget(long long targetMs);

    // 视频帧渲染回调（std::function 本体，不是指针；
    // 传进 CMD_OPEN 时按值拷贝，Player 自己活得比它久）
    PaintFrame m_videoCb;
    AudioPlayer* m_auPlayer;

    //这个变量是为了处理短时间快速seek的导致的Core还未处理上次seek导致出现按键冲突
    //发信息时置false，seekDone时置true
    bool m_canSeek = true;
    //m_canSeek 的兜底截止时刻：万一本该来的 EVT_SEEK_DONE 丢了（比如 doSeek 早退），
    //到点就自动解锁，绝不把快进/快退永久锁死。
    qint64 m_canSeekUntilMs = 0;

    bool m_isPause = false;

    bool isSliderPress = false;
    //DemuxThread dt;

    bool m_isInit = false;


    // —— 快进/快退去抖：短时间内连续按键只累积，停下来才真正 seek 一次 ——
    // 待生效的 seek 目标（毫秒），-1 表示当前没有待生效目标
    long long m_pendingSeekMs = -1;
    // 去抖定时器 id（0 = 未启动）
    int m_seekDebounceTimerId = 0;

    /*
     * 进度条“钉在目标上”的状态（两者成对使用，m_seekTargetMs < 0 表示没在钉）。
     *
     * 用户刚发起 seek 时，进度条该显示“目标位置”；而 core 的 EVT_POSITION 约 150ms 一次、
     * 报的是真实时钟，直接覆盖就会把握在手里的目标顶回去 —— 长按快进/快退时表现为
     * 进度条在“目标”和“当前”之间来回跳。
     *
     * 解除条件（下一次 EVT_POSITION 时判）：
     *   - 时钟已经走到目标附近（|当前 - 目标| <= kSeekLandToleranceMs）→ seek 已落地；
     *   - 或者超过 m_seekUiHoldUntilMs（兜底，防止 SEEK_DONE 丢了把进度条永久冻住）。
     * 用户不在连续操作时，onSEEkDONE() 收到落地事件会直接校正显示。
     *
     * m_seekTargetMs 同时还是“连按累积”的基准：连按期间 m_pts 追不上已经显示的目标，
     * 下一次按键必须接着目标加，而不是接着旧时钟加。
     */
    long long m_seekTargetMs = -1;
    qint64    m_seekUiHoldUntilMs = 0;

    /*
     * “用户正在连续 seek”的时间窗（截止时刻），每次快进/快退/拖动都刷新。
     * 窗口内收到 EVT_SEEK_DONE 只更新内部位置，**不去改进度条**：
     * 连按时进度条已经由按键驱动到目标上了，再被“精准落点”写一次就会出现
     * “目标 → 落点 → 下一个目标”的来回抖。窗口结束后显示交给 EVT_POSITION 收敛。
     */
    qint64 m_seekActiveUntilMs = 0;

    bool m_isClosing = false;

    double m_nowVolume = 0.5;

    int m_lastW = 0;
    int m_lastH = 0;

    int m_lastVideoW = 0;
    int m_lastVideoH = 0;


    int m_videoSrcW = 0;   // 原始视频宽
    int m_videoSrcH = 0;   // 原始视频高

    bool m_isFullScreen = false;

    //是否最大化
    bool m_isMaximum = false;

    //最大化按钮标志 用于列表存在时控制窗口大小
    bool m_maximunFlag = false;

    int m_playListWidth = 200;

    bool is_playListVisible = false;

    //列表按钮标志 用于列表存在时控制窗口大小
    bool m_listBtnOn = false;

    // 保存非最大化状态的窗口位置+尺寸
    QRect m_normalGeo;

    bool m_isPrevFramePlay = false;

    bool m_stepFrame = false;

    double m_speed = 1.0;

    long long m_durationMs = 0;

    //用于显示进度条和供快进快退使用，大致有50~200ms的误差，可以接受
    long long m_pts = 0;

};
#endif // PLAYER_H
