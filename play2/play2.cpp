
#pragma execution_character_set("utf-8")
#include "Play2.h"
#include <QFileDialog>
#include <QDebug>
#include "DemuxThread.h"
#include <QMessageBox>
static DemuxThread dt;

Play2::Play2(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    dt.Start();
    startTimer(40);
    connect(ui.openFile, &QPushButton::clicked, this, &Play2::OpenFile);
    connect(ui.isplay, &QPushButton::clicked, this, &Play2::PlayOrPause);
    connect(ui.playPos, &Slider::sliderPressed, this, &Play2::SliderPress);
    connect(ui.playPos, &Slider::sliderReleased, this, &Play2::SliderRelease);

}
Play2::~Play2()
{
    dt.Close();
}
void Play2::SliderPress()
{
    isSliderPress = true;
}
void Play2::SliderRelease()
{
    isSliderPress = false;
    double pos = 0.0;
    pos = (double)ui.playPos->value() / (double)ui.playPos->maximum();
    dt.Seek(pos);
}
//定时器 滑动条显示
void Play2::timerEvent(QTimerEvent *e)
{
    if (isSliderPress)return;
    long long total = dt.totalMs;
    if (total > 0)
    {
        double pos = (double)dt.pts / (double)total;
        int v = ui.playPos->maximum() * pos;
        ui.playPos->setValue(v);
    }
}
//双击全屏
void Play2::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (isFullScreen())
        this->showNormal();
    else
        this->showFullScreen();
}
//窗口尺寸变化
void Play2::resizeEvent(QResizeEvent *e)
{
    ui.playPos->move(50, this->height() - 100);
    ui.playPos->resize(this->width() - 100, ui.playPos->height());
    ui.openFile->move(100,this->height() - 150);
    ui.isplay->move(ui.openFile->x() + ui.openFile->width() + 10, ui.openFile->y());
    ui.video->resize(this->size());
}

void Play2::PlayOrPause()
{
    bool isPause = !dt.isPause;
    SetPause(isPause);
    dt.SetPause(isPause);

}

void Play2::SetPause(bool isPause)
{
    if (isPause)
        ui.isplay->setText("播 放");
    else
        ui.isplay->setText("暂 停");
}



void Play2::OpenFile()
{
    //选择文件
    QString name = QFileDialog::getOpenFileName(this, "选择视频文件");
    if (name.isEmpty())return;
    this->setWindowTitle(name);
    if (!dt.Open(name.toUtf8().constData(), ui.video))
    {
        QMessageBox::information(0, "error", "open file failed!");
        return;
    }
    SetPause(dt.isPause);

}
