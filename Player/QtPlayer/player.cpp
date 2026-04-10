#include "player.h"
#include "./ui_player.h"
#include "myslider.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>

Player::Player(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Player)
{
    ui->setupUi(this);
    // 1. 设置VideoWidget大小策略为自动扩展
    ui->video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 2. 把VideoWidget放到最底层，防止被其他控件挡住
    ui->video->lower();
    ui->btnWidget->raise();
    // 3. 初始就铺满整个窗口
    resize(1280, 720);              // 默认窗口大小（更友好）
    setMinimumSize(640, 360);       // 最小限制（16:9）
    setMaximumSize(1920, 1080);     // 最大限制（可选）
    ui->video->setGeometry(0, 0, width(), height());



    dt.start();



    connect(ui->slider, &mySlider::sliderPressed, this, &Player::sliderPress);
    connect(ui->slider, &mySlider::sliderReleased, this, &Player::sliderRelease);
    connect(&dt,&DemuxThread::disableBtn,this,[=](){
        bool isPause = dt.getIsPause();
        setPauseText(isPause);
        ui->btnStart->setDisabled(true);
        ui->openFile->setDisabled(true);
    });
    connect(&dt,&DemuxThread::ableBtn,this,[=](){
        bool isPause = dt.getIsPause();
        setPauseText(isPause);
        ui->btnStart->setDisabled(false);
        ui->openFile->setDisabled(false);
    });
}

Player::~Player()
{
    dt.close();
    delete ui;
}

void Player::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);

    // 视频铺满整个窗口
    ui->video->setGeometry(0, 0, width(), height());
    ui->video->raise();   // 视频在最底层
    ui->video->lower();

    // 控件永远在最上层
    ui->slider->raise();
    ui->btnWidget->raise();

    // 重新布局
    int btnY = height() - 80;
    ui->btnWidget->move((width() - ui->btnWidget->width())/2, btnY);

    int sliderY = height() - 30;
    ui->slider->setGeometry(50, sliderY, width() - 100, 20);
}

void Player::mouseDoubleClickEvent(QMouseEvent *e)
{
    if(isFullScreen()){
        this->showNormal();
    }else{
        this->showFullScreen();
    }
    ui->slider->raise();
    ui->btnWidget->raise();
}



void Player::on_openFile_clicked()
{
    //选择文件
    QString name = QFileDialog::getOpenFileName(this,"选择视频文件");
    if(name.isEmpty()){
        QMessageBox::information(this,"error","file is empty");
    }
    this->setWindowTitle(name);
    if(!dt.openFile(name.toUtf8().constData(),ui->video)){
        QMessageBox::information(0, "error", "open file failed!");
        return;
    }
    this->showNormal();
    m_isInit = true;
    // 重置滑块状态，避免之前操作的影响
    isSliderPress = false;                 // 清除按下标志
    ui->slider->setValue(0);               // 滑块归零
    ui->slider->setMaximum(dt.totalMs);    // 设置新的最大值（毫秒
    startTimer(16);
    if(dt.m_width==1920&&dt.m_height==1080){
        resize(1280, 720);
    }else{
        resize(dt.m_width/2, dt.m_height/2);
    }
    update();
    setPauseText(dt.getIsPause());
}


void Player::on_btnStart_clicked()
{
    bool isPause = !dt.getIsPause();
    setPauseText(isPause);
    dt.setPause(isPause);
}



void Player::setPauseText(bool isPause)
{
    if(isPause){
        ui->btnStart->setText(QString("播放"));
    }else{
        ui->btnStart->setText(QString("暂停"));
    }
}

void Player::sliderPress()
{
    isSliderPress = true;
}

void Player::sliderRelease()
{
    if(!m_isInit){
        return;
    }
    isSliderPress = false;
    double pos = 0.0;
    pos = (double)ui->slider->value() / (double)ui->slider->maximum();
    dt.seek(pos);
}

void Player::timerEvent(QTimerEvent *e)
{
    if (isSliderPress)return;
    long long total = dt.totalMs;
    if (total > 0)
    {
        // double pos = (double)dt.pts / (double)total;
        // int v = ui->slider->maximum() * pos;
        // ui->slider->setValue(v);
        ui->slider->setMaximum(total);
        ui->slider->setValue(dt.getVideoPts());
    }
}
