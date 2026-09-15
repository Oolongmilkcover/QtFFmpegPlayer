#include "topmenu.h"
#include "ui_topmenu.h"
#include <QMouseEvent>
#include <QFileInfo>
#include <QGuiApplication>   // 只在 Wayland 才需要请系统接管窗口移动
#include <QWindow>           // startSystemMove

TopMenu::TopMenu(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::TopMenu)
{
    ui->setupUi(this);
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
    ui->closeBtn->setIcon(QIcon(":/workBtnPNG/closeBtn.png"));
    ui->closeBtn->setIconSize(ui->closeBtn->size());

    ui->fullOrBackBtn->setIcon(QIcon(":/workBtnPNG/maxBtn.png"));
    ui->fullOrBackBtn->setIconSize(ui->fullOrBackBtn->size());

    ui->hideWindowBtn->setIcon(QIcon(":/workBtnPNG/miniBtn.png"));
    ui->hideWindowBtn->setIconSize(ui->hideWindowBtn->size());

    ui->openFileBtn->setIcon(QIcon(":/workBtnPNG/openFileBtn.png"));
    ui->openFileBtn->setIconSize(ui->openFileBtn->size());

    setNoPlayText();

    ui->playerName->setAttribute(Qt::WA_TransparentForMouseEvents);
    ui->fileName->setAttribute(Qt::WA_TransparentForMouseEvents);

    for (QPushButton *btn : findChildren<QPushButton*>()) {
        btn->setFocusPolicy(Qt::NoFocus);
    }
}

TopMenu::~TopMenu()
{
    delete ui;
}

void TopMenu::setPlayingText(QString name)
{
    ui->fileName->setText(getPlayFileName(name));
}

QString TopMenu::getPlayFileName(const QString &filePath)
{
    QFileInfo fi(filePath);
    return QString("正在播放:")+fi.fileName();
}

void TopMenu::setNoPlayText()
{
    ui->fileName->setText("暂未播放");
}

void TopMenu::maxOrRestoreChange()
{
    if(m_isWindowMax){
        ui->fullOrBackBtn->setIcon(QIcon(":/workBtnPNG/maxBtn.png"));
        ui->fullOrBackBtn->setIconSize(ui->fullOrBackBtn->size());
        m_isWindowMax = false;
    }
}

void TopMenu::stepFrameTime(bool flag)
{
    ui->openFileBtn->setDisabled(flag);
}



void TopMenu::on_openFileBtn_clicked()
{
    emit openFile();
}


void TopMenu::on_closeBtn_clicked()
{
    emit closeClicked();
}


void TopMenu::on_fullOrBackBtn_clicked()
{
    if(!m_isWindowMax){
        ui->fullOrBackBtn->setIcon(QIcon(":/workBtnPNG/restoreBtn.png"));
        ui->fullOrBackBtn->setIconSize(ui->fullOrBackBtn->size());
        emit maximization();
    }else{
        ui->fullOrBackBtn->setIcon(QIcon(":/workBtnPNG/maxBtn.png"));
        ui->fullOrBackBtn->setIconSize(ui->fullOrBackBtn->size());
        emit restore();
    }
    m_isWindowMax = !m_isWindowMax;
}


void TopMenu::on_hideWindowBtn_clicked()
{
    emit hideWindow();
}


void TopMenu::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
    {
        /*
         * 只有 Wayland 需要请系统接管窗口移动：客户端在那里不能自己 move
         * 顶层窗口（win->move() 会被合成器忽略），只能请求交互式移动。
         *
         * X11 / Windows 保持自绘拖动（和 v2.1 一致）：它们允许客户端自己 move，
         * 而且自绘走正常事件循环；原生移动是模态循环，期间 Qt 不跑事件循环，
         * 拖窗口时画面会停住，看起来就是卡顿。
         * 必须在鼠标按下的处理里同步调用。
         */
        if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
            QWindow *wh = window()->windowHandle();
            if (wh && wh->startSystemMove()) {
                m_isDragging = false;
                QWidget::mousePressEvent(e);
                return;
            }
        }

        m_isDragging = true;
        // 记录鼠标按下点相对顶层窗口左上角的偏移
        m_dragOffset = e->globalPosition().toPoint()
                       - window()->frameGeometry().topLeft();
    }
    QWidget::mousePressEvent(e);
}
void TopMenu::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_isDragging || !(e->buttons() & Qt::LeftButton))
        return;
    QWidget *win = window();
    if (win->isMaximized())
    {
        // 先还原，并把窗口放到鼠标位置附近
        //win->showNormal();
        QPoint g = e->globalPosition().toPoint();
        win->move(g.x() - m_dragOffset.x(), g.y() - 5);
        // 重新记偏移，避免窗口跳动
        m_dragOffset = e->globalPosition().toPoint()
                       - win->frameGeometry().topLeft();
        return;
    }
    win->move(e->globalPosition().toPoint() - m_dragOffset);
    if(m_isWindowMax){
        ui->fullOrBackBtn->setIcon(QIcon(":/workBtnPNG/maxBtn.png"));
        ui->fullOrBackBtn->setIconSize(ui->fullOrBackBtn->size());
        m_isWindowMax = false;
    }


}
void TopMenu::mouseReleaseEvent(QMouseEvent *e)
{
    m_isDragging = false;
    QWidget::mouseReleaseEvent(e);
}

void TopMenu::keyPressEvent(QKeyEvent *e)
{
    return;
}

void TopMenu::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    //maxOrRestoreChange();
}
