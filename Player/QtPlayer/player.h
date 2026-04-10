#ifndef PLAYER_H
#define PLAYER_H
#include "demuxthread.h"
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
    void resizeEvent(QResizeEvent * e);
    //双击切换全屏
    void mouseDoubleClickEvent(QMouseEvent *e);
    //进度条动态显示 定时器
    void timerEvent(QTimerEvent *e);
private slots:
    void on_openFile_clicked();

    void on_btnStart_clicked();

    void setPauseText(bool isPause);

    //进度条
    void sliderPress();
    void sliderRelease();

private:
    Ui::Player *ui;

    bool isSliderPress = false;
    DemuxThread dt;
    bool m_isInit = false;
};
#endif // PLAYER_H
