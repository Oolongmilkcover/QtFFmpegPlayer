#pragma once
#include <QtWidgets/QWidget>
#include "ui_play2.h"

class Play2 : public QMainWindow
{
    Q_OBJECT

public:
    Play2(QWidget *parent = Q_NULLPTR);
    ~Play2();

    //定时器 滑动条显示
    void timerEvent(QTimerEvent *e);

    //窗口尺寸变化
    void resizeEvent(QResizeEvent *e);


    //双击全屏
    void mouseDoubleClickEvent(QMouseEvent *e);
    void SetPause(bool isPause);

public slots:
    void OpenFile();
    void PlayOrPause();
    void SliderPress();
    void SliderRelease();
private:
    bool isSliderPress = false;
    Ui::play2 ui;
};
