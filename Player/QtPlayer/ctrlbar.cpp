#include "ctrlbar.h"
#include "ui_ctrlbar.h"
#include<QDebug>
#include<QMenu>
CtrlBar::CtrlBar(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::CtrlBar)
{
    ui->setupUi(this);

    //除去按钮的轮廓
    qApp->setStyleSheet(R"(
        QPushButton{
            border: none;
            background: none;
            outline: none;
        }
        QPushButton:hover,QPushButton:pressed{
            border: none;
            background: none;
        }
        )");

    for (QPushButton *btn : findChildren<QPushButton*>()) {
        btn->setFocusPolicy(Qt::NoFocus);
    }

    ui->nextBtn->setIcon(QIcon(":/workBtnPNG/nextBtn.png"));
    ui->nextBtn->setIconSize(ui->nextBtn->size());

    ui->stopBtn->setIcon(QIcon(":/workBtnPNG/stopBtn.png"));
    ui->stopBtn->setIconSize(ui->stopBtn->size());

    ui->playOrPauseBtn->setIcon(QIcon(":/workBtnPNG/startBtn.png"));
    ui->playOrPauseBtn->setIconSize(ui->playOrPauseBtn->size());

    ui->prevBtn->setIcon(QIcon(":/workBtnPNG/prevBtn.png"));
    ui->prevBtn->setIconSize(ui->prevBtn->size());

    ui->slowdownBtn->setIcon(QIcon(":/workBtnPNG/slowdownBtn.png"));
    ui->slowdownBtn->setIconSize(ui->slowdownBtn->size());

    ui->speedupBtn->setIcon(QIcon(":/workBtnPNG/speedupBtn.png"));
    ui->speedupBtn->setIconSize(ui->speedupBtn->size());

    ui->volume->setIcon(QIcon(":/workBtnPNG/volumeBtn.png"));
    ui->volume->setIconSize(ui->volume->size());

    ui->settingBtn->setIcon(QIcon(":/workBtnPNG/settingBtn.png"));
    ui->settingBtn->setIconSize(ui->settingBtn->size());

    ui->playListBtn->setIcon(QIcon(":/workBtnPNG/showlistBtn.png"));
    ui->playListBtn->setIconSize(ui->playListBtn->size());

    ui->fullPlayBtn->setIcon(QIcon(":/workBtnPNG/fullBtn.png"));
    ui->fullPlayBtn->setIconSize(ui->playListBtn->size());

    ui->nextFrameBtn->setIcon(QIcon(":/workBtnPNG/nextFrameBtn.png"));
    ui->nextFrameBtn->setIconSize(ui->playListBtn->size());

    ui->prevFrameBtn->setIcon(QIcon(":/workBtnPNG/prevFrameBtn.png"));
    ui->prevFrameBtn->setIconSize(ui->playListBtn->size());

    ui->ffBtn->setIcon(QIcon(":/workBtnPNG/ffBtn.png"));
    ui->ffBtn->setIconSize(ui->playListBtn->size());

    ui->rewindBtn->setIcon(QIcon(":/workBtnPNG/rewindBtn.png"));
    ui->rewindBtn->setIconSize(ui->playListBtn->size());

    ui->volumeSlider->setMaximum(100);
    ui->volumeSlider->setValue(50);

    connect(ui->playSlider, &mySlider::sliderPressed, this, &CtrlBar::sliderPress);
    connect(ui->playSlider, &mySlider::sliderReleased, this, &CtrlBar::sliderRelease);
    connect(ui->volumeSlider, &mySlider::sliderPressed, this, &CtrlBar::volumeSliderPress);
    connect(ui->volumeSlider, &mySlider::sliderReleased, this, &CtrlBar::volumeSliderRelease);
}

CtrlBar::~CtrlBar()
{
    delete ui;
}

void CtrlBar::sliderPress()
{
    m_isSliderPress = true;
}

void CtrlBar::sliderRelease()
{
    m_isSliderPress = false;
    if(!m_isSliderInit){
        return;
    }
    double pos = 0.0;
    pos = (double)ui->playSlider->value() / (double)ui->playSlider->maximum();
    emit seek(pos);
}

void CtrlBar::volumeSliderPress()
{
    m_isVolumeSliderPress = true;
}

void CtrlBar::volumeSliderRelease()
{
    m_isVolumeSliderPress = false;
    double pos = 0.0;
    pos = (double)ui->volumeSlider->value() / (double)ui->volumeSlider->maximum();
    emit setVolume(pos);
    if(pos==0.0){
        ui->volume->setIcon(QIcon(":/workBtnPNG/volumeNoneBtn.png"));
        ui->volume->setIconSize(ui->volume->size());
    }else{
        ui->volume->setIcon(QIcon(":/workBtnPNG/volumeBtn.png"));
        ui->volume->setIconSize(ui->volume->size());
    }
}

void CtrlBar::on_playOrPauseBtn_clicked()
{

    if(m_isPause){
        m_isPause = !m_isPause;
        emit play();
    }else{
        m_isPause = !m_isPause;
        emit pause();
    }

}


void CtrlBar::on_stopBtn_clicked()
{
    emit stop();
}


void CtrlBar::on_prevBtn_clicked()
{
    emit prev();
}


void CtrlBar::on_nextBtn_clicked()
{
    emit next();
}


void CtrlBar::on_slowdownBtn_clicked()
{
    m_speed -= 0.1;
    if (m_speed < 0.5) m_speed = 0.5;
    ui->speedLabel->setText(QString("倍速:%1").arg(m_speed));
    emit speedChanged(-0.1);
}


void CtrlBar::on_speedupBtn_clicked()
{
    m_speed += 0.1;
    if (m_speed > 2.0) m_speed = 2.0;
    ui->speedLabel->setText(QString("倍速:%1").arg(m_speed));
    emit speedChanged(0.1);
}


void CtrlBar::on_playListBtn_clicked()
{
    emit showOrHidePlayList();
}


void CtrlBar::on_settingBtn_clicked()
{
    QMenu menu(this);
    // 菜单项：原色/灰度/反色/暖色/冷色
    QAction *actNormal = menu.addAction("原色");
    QAction *actGray   = menu.addAction("灰度");
    QAction *actInvert = menu.addAction("反色");
    QAction *actWarm   = menu.addAction("暖色");
    QAction *actCool   = menu.addAction("冷色");
    // 在按钮下方弹出菜单
    QAction *chosen = menu.exec(ui->settingBtn->mapToGlobal(
        QPoint(0, ui->settingBtn->height())));
    // 判断选中了哪个
    if (chosen == actNormal) emit filterChanged(0);
    else if (chosen == actGray)   emit filterChanged(1);
    else if (chosen == actInvert) emit filterChanged(2);
    else if (chosen == actWarm)   emit filterChanged(3);
    else if (chosen == actCool)   emit filterChanged(4);
}

QString CtrlBar::msToString(int ms)
{
    qint64 totalSec = ms / 1000;
    qint64 h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    // %02d 补零，保证 0:5:3 → "00:05:03"
    return QString("%1:%2:%3")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'));
}

void CtrlBar::setPausePictrue(bool isPause)
{
    if(!isPause){
        ui->playOrPauseBtn->setIcon(QIcon(":/workBtnPNG/pauseBtn.png"));
        ui->playOrPauseBtn->setIconSize(ui->playOrPauseBtn->size());
    }else{
        ui->playOrPauseBtn->setIcon(QIcon(":/workBtnPNG/startBtn.png"));
        ui->playOrPauseBtn->setIconSize(ui->playOrPauseBtn->size());
    }
    m_isPause = isPause;
}

void CtrlBar::setSliderValue(int value)
{
    if(m_isSliderPress) return;
    ui->playSlider->setValue(value);
    ui->playTimeEdit->setText(msToString(value));
}

int CtrlBar::getSliderValue()
{
    return ui->playSlider->value();
}

void CtrlBar::setSliderMaximum(int maximum)
{
    if(maximum==0){
        m_isSliderInit = false;
    }else{
        m_isSliderInit = true;
    }
    ui->playSlider->setMaximum(maximum);
    ui->totalTimeEdit->setText(msToString(maximum));
}

int CtrlBar::getSliderMaximum() const
{
    return ui->playSlider->maximum();
}

bool CtrlBar::getSliderPress()
{
    return m_isSliderPress;
}

void CtrlBar::stepFrameTime(bool flag)
{

    //ui->playOrPauseBtn->setDisabled(flag);
    //ui->nextFrameBtn->setDisabled(flag);
    //ui->prevFrameBtn->setDisabled(flag);
    ui->stopBtn->setDisabled(flag);
    ui->prevBtn->setDisabled(flag);
    ui->nextBtn->setDisabled(flag);
    ui->settingBtn->setDisabled(flag);
    ui->fullPlayBtn->setDisabled(flag);
    ui->ffBtn->setDisabled(flag);
    ui->rewindBtn->setDisabled(flag);
    ui->speedupBtn->setDisabled(flag);
    ui->slowdownBtn->setDisabled(flag);
    ui->playListBtn->setDisabled(flag);

}

void CtrlBar::setSpeedLabel(double speed)
{
    ui->speedLabel->setText(QString("倍速:%1").arg(speed));
    m_speed = speed;   // 同步内部状态
}

void CtrlBar::setVolumeSlider(int value)
{
    if (m_isVolumeSliderPress) return;   // 用户正拖音量条时不强制更新
    ui->volumeSlider->setValue(value);
}

void CtrlBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}



void CtrlBar::on_fullPlayBtn_clicked()
{
    emit fullPlay();
}


void CtrlBar::on_ffBtn_clicked()
{
    emit ff();
}


void CtrlBar::on_rewindBtn_clicked()
{
    emit rewind();
}


void CtrlBar::on_prevFrameBtn_clicked()
{
    emit prevFrame(2);
}


void CtrlBar::on_nextFrameBtn_clicked()
{
    emit nextFrame(1);
}

