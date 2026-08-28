#ifndef PLAYER_H
#define PLAYER_H
#include "demuxthread.h"
#include <QListWidgetItem>
#include <QWidget>

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

signals:
    void setPausePicture(bool isPause);

    void setPlayingText(QString name);

    void maxOrRestore();
private slots:
    //进度条
    void sliderSeek(double pos);

    void on_openFile_clicked();

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

    void on_playList_doubleClicked(QListWidgetItem *item);

    void ffSeekFiveSec();

    void rewindSeekFiveSec();

    void stepFrame(int mode);
private:
    Ui::Player *ui;

    bool isSliderPress = false;
    DemuxThread dt;

    bool m_isInit = false;

    int m_timerId = 0;

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
};
#endif // PLAYER_H
