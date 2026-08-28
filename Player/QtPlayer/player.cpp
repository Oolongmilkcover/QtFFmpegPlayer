#include "player.h"
#include "ui_player.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QScreen>
#include <QPaintEvent>
#include <QPainter>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
Player::Player(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Player)
{
    ui->setupUi(this);
    //去掉标题和原生按钮
    this->setWindowFlags(Qt::FramelessWindowHint);
    // 设置VideoWidget大小策略为自动扩展
    ui->video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 初始就铺满整个窗口
    resize(1280+12, 720+ui->ctrlbar->height()+ui->topMenu->height()+12);              // 默认窗口大小（更友好）
    setMinimumSize(432+12, 243+ui->ctrlbar->height()+ui->topMenu->height()+12);       // 最小限制（16:9）
    ui->video->setFixedSize(1280, 720);

    //边缘缩放
    ui->video->installEventFilter(this);
    ui->ctrlbar->installEventFilter(this);

    ui->video->setGeometry(0, 0, width(), height());

    //播放列表初始化
    ui->dockWidget->setFeatures(QDockWidget::NoDockWidgetFeatures);
    ui->dockWidget->setFixedWidth(m_playListWidth);
    ui->dockWidget->hide();
    ui->dockWidget->setStyleSheet("QDockWidget::title { height:0; }");
    ui->dockWidget->setFloating(false);
    ui->dockWidget->setAllowedAreas(Qt::NoDockWidgetArea);
    ui->dockWidget->setWindowTitle("播放列表");

    connect(&dt,&DemuxThread::disableBtn,this,[=](){
        bool isPause = dt.getIsPause();
        setPausePicture(isPause);
        ui->ctrlbar->setDisabled(true);
        ui->topMenu->setDisabled(true);
    });
    connect(&dt,&DemuxThread::ableBtn,this,[=](){
        bool isPause = dt.getIsPause();
        setPausePicture(isPause);
        ui->ctrlbar->setDisabled(false);
        ui->topMenu->setDisabled(false);
    });


    //底部控制按键组信号操作
    //开始与暂停按钮控制
    connect(ui->ctrlbar, &CtrlBar::play, this,[this]{
        ui->ctrlbar->stepFrameTime(false);
        ui->topMenu->stepFrameTime(false);
        m_isPrevFramePlay = false;
        m_stepFrame = false;
        dt.setPause(false);
    });
    connect(ui->ctrlbar, &CtrlBar::pause, this,[this]{dt.setPause(true);});
    connect(ui->ctrlbar, &CtrlBar::setVolume, this,&Player::setVolume);
    connect(this,&Player::setPausePicture,ui->ctrlbar, &CtrlBar::setPausePictrue);
    //停止播放
    connect(ui->ctrlbar, &CtrlBar::stop, this,&Player::stopToPlay);
    //进度条移动和seek操作
    connect(ui->ctrlbar, &CtrlBar::seek, this,&Player::sliderSeek);
    //全屏
    connect(ui->ctrlbar, &CtrlBar::fullPlay, this, &Player::toggleFullScreen);
    //打开播放列表
    connect(ui->ctrlbar, &CtrlBar::showOrHidePlayList, this, &Player::showOrHidePlayList);
    //打开设置

    //下一集
    connect(ui->ctrlbar, &CtrlBar::next, this, &Player::playNext);
    //上一集
    connect(ui->ctrlbar, &CtrlBar::prev, this, &Player::playPrev);
    //快进5s
    connect(ui->ctrlbar, &CtrlBar::ff, this, &Player::ffSeekFiveSec);
    //快退5s
    connect(ui->ctrlbar, &CtrlBar::rewind, this, &Player::rewindSeekFiveSec);
    //下一帧
    connect(ui->ctrlbar, &CtrlBar::nextFrame, this, &Player::stepFrame);
    //上一帧
    connect(ui->ctrlbar, &CtrlBar::prevFrame, this, &Player::stepFrame);


    //---------------------------------------------------------------------------
    //顶部按键组
    connect(ui->topMenu, &TopMenu::openFile, this,&Player::on_openFile_clicked);
    connect(ui->topMenu, &TopMenu::closeClicked, this, &QWidget::close);
    connect(ui->topMenu, &TopMenu::hideWindow, this,[this]{this->showMinimized();});
    connect(ui->topMenu, &TopMenu::maximization, this,[this]{
        m_normalGeo = this->geometry();
        setGeometry(screen()->availableGeometry());
        m_isMaximum = true;
        m_maximunFlag = true;});
    connect(ui->topMenu, &TopMenu::restore, this,[this]{
        this->setGeometry(m_normalGeo);
        m_isMaximum = false;
        m_maximunFlag = true;
    });
    connect(this,&Player::setPlayingText,ui->topMenu, &TopMenu::setPlayingText);
    connect(this,&Player::maxOrRestore,ui->topMenu, &TopMenu::maxOrRestoreChange);




    //---------------------------------------------------------------------------
    //播放列表
    //播这个
    connect(ui->playList->getListWidget(), &QListWidget::itemDoubleClicked,
            this, &Player::on_playList_doubleClicked);
    connect(ui->playList, &PlayList::playThis,
            this, &Player::on_playList_doubleClicked);

    //来自dt的下一集
    connect(&dt, &DemuxThread::playNext, this, &Player::playNext);
    connect(ui->playList,&PlayList::noMoreToPlay,this,[this]{dt.setHasPlayList(false);});

    //支持拖文件进来
    setAcceptDrops(true);
}

Player::~Player()
{
    if (!m_isClosing) dt.close();
    if (m_timerId) killTimer(m_timerId);
    delete ui;
}

void Player::resizeEvent(QResizeEvent *e)
{
    if(m_isMaximum && !m_maximunFlag){
        emit maxOrRestore();
        m_isMaximum = false;
    }
    m_lastW = this->width();
    m_lastH = this->height();
    QWidget::resizeEvent(e);
    if(is_playListVisible && !m_maximunFlag){
        if(m_listBtnOn && !m_maximunFlag){
            m_listBtnOn  = false;
            return;
        }
    }
    m_listBtnOn  = false;
    m_maximunFlag = false;
    int playW = this->width() -12;
    if(is_playListVisible){
        playW -= m_playListWidth;
    }
    int playH = this->height()
                - ui->topMenu->height()
                - ui->ctrlbar->height()-12;
    if (playW <= 0 || playH <= 0){
        ui->video->setFixedSize(0, 0);
        return;
    };
    double srcAspect;
    if (m_videoSrcW <= 0 || m_videoSrcH <= 0){
        srcAspect = (double)16/9;
    }else{
         srcAspect = (double)m_videoSrcW / m_videoSrcH;
    }
    double boxAspect = (double)playW / playH;
    int newW, newH;
    if (boxAspect > srcAspect) {
        newH = playH;
        newW = playH * srcAspect;
    } else {
        newW = playW;
        newH = playW / srcAspect;
    }
    m_lastVideoW = newW;
    m_lastVideoH = newH;
    ui->video->setFixedSize(newW, newH);
}

void Player::mouseDoubleClickEvent(QMouseEvent *e)
{
   toggleFullScreen();
}



void Player::on_openFile_clicked()
{
    QString name = QFileDialog::getOpenFileName(this, "选择视频文件");
    if (name.isEmpty()) return;
    //addToPlayList(name);   // 打开也加入列表
    playFile(name);
}

void Player::setVolume(double pos)
{
    if(m_isInit){
        dt.setVolume(pos);
    }
    m_nowVolume = pos;
}

void Player::toggleFullScreen()
{
    if(m_videoSrcW == 0 || m_videoSrcH == 0 ){
        return;
    }
    if (!m_isFullScreen) {
        // 进入全屏
        m_isFullScreen = true;
        ui->topMenu->hide();
        ui->ctrlbar->hide();
        ui->dockWidget->hide();
        showFullScreen();
        //抹去间隔
        this->layout()->setContentsMargins(0,0,0,0);
        ui->video->setFixedSize(m_videoSrcW,m_videoSrcH);

    } else {
        // 退出全屏
        m_isFullScreen = false;
        ui->topMenu->show();
        ui->ctrlbar->show();
        //添加间隔以便缩放
        this->layout()->setContentsMargins(6,6,6,6);
        showNormal();
        ui->video->setFixedSize(m_lastVideoW,m_lastVideoH);
        if (is_playListVisible) {
            ui->dockWidget->show();
        }
    }
}

void Player::stopToPlay()
{
    setPausePicture(true);
    dt.close();
    ui->video->clearScreen();
    ui->ctrlbar->setDisabled(true);
    ui->topMenu->setNoPlayText();
    m_videoSrcH = 0;
    m_videoSrcW = 0;
    m_isInit = false;
}



void Player::showOrHidePlayList()
{
    m_listBtnOn = true;
    if (is_playListVisible) {
        ui->dockWidget->hide();
        is_playListVisible = !is_playListVisible;
        setMinimumSize(432+12, 243+ui->ctrlbar->height()+ui->topMenu->height()+12);
        if(m_isMaximum){
            this->showMaximized();
        }else{
            resize(m_lastW-m_playListWidth,m_lastH);
        }
    } else {
        is_playListVisible = !is_playListVisible;
        setMinimumSize(432+12+m_playListWidth, 243+ui->ctrlbar->height()+ui->topMenu->height()+12);
        //int newW = this->width() + m_playListWidth;
        int newW = m_lastW + m_playListWidth;
        if(m_lastVideoW <= 432){
            newW = m_lastW;
        }

        int newH = this->height();
        if(m_isMaximum){
            newW = this->width();
            ui->dockWidget->show();
            resize(newW,newH);
        }else{
            resize(newW,newH);
            ui->dockWidget->show();
        }
    }
}

void Player::playFile(const QString &path)
{
    if (path.isEmpty()||m_stepFrame) return;
    this->setWindowTitle(path);
    if (!dt.openFile(path.toUtf8().constData(), ui->video)) {
        QMessageBox::information(0, "error", "open file failed!");
        return;
    }
    emit setPlayingText(path);
    dt.start();
    this->showNormal();
    m_isInit = true;
    // 重置滑块状态，避免之前操作的影响
    isSliderPress = false;                 // 清除按下标志
    ui->ctrlbar->setSliderValue(0);            // 滑块归零
    ui->ctrlbar->setSliderMaximum(dt.totalMs);
    ui->ctrlbar->setDisabled(false);
    setVolume(m_nowVolume);
    if (m_timerId)
        killTimer(m_timerId);
    //用于进度条的计时器？
    m_timerId = startTimer(16);
    m_videoSrcW = dt.m_width;
    m_videoSrcH = dt.m_height;
    update();
    setPausePicture(dt.getIsPause());
}

void Player::playNext()
{
    QListWidget *list = ui->playList->getListWidget();
    if (list->count() == 0) return;
    ui->playList->next();                              // 环形选中下一行
    QString path = ui->playList->itemPath(ui->playList->currentRow());
    if (!path.isEmpty()) playFile(path);
}

void Player::playPrev()
{
    QListWidget *list = ui->playList->getListWidget();
    if (list->count() == 0) return;
    ui->playList->prev();                              // 环形选中上一行
    QString path = ui->playList->itemPath(ui->playList->currentRow());
    if (!path.isEmpty()) playFile(path);
}

void Player::on_playList_doubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    ui->playList->setCurrentRow(ui->playList->getListWidget()->row(item));
    QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty()) playFile(path);
}

void Player::ffSeekFiveSec()
{
    if(!m_isInit){
        return;
    }
    long long totalMs = dt.totalMs;
    long long seekPtsMs =  (dt.getVideoPts()+5000) > totalMs ? totalMs : (dt.getVideoPts()+5000);
    double pos = (double)seekPtsMs / totalMs;
    dt.seek(pos);
}

void Player::rewindSeekFiveSec()
{
    if(!m_isInit){
        return;
    }
    long long totalMs = dt.totalMs;
    long long seekPtsMs =  (dt.getVideoPts()- 5000) < 0 ? 0 : (dt.getVideoPts()- 5000);
    double pos = (double)seekPtsMs / totalMs ;
    dt.seek(pos);
}



void Player::stepFrame(int mode)
{
    if(!m_isInit) return;
    ui->ctrlbar->setPausePictrue(true);
    //按钮设置不可用
    ui->ctrlbar->stepFrameTime(true);
    ui->topMenu->stepFrameTime(true);
    if(mode == 1){
        m_isPrevFramePlay = false;
        m_stepFrame = true;
        dt.stepNextFrame();
    }else{
        if(m_isPrevFramePlay) return;
        m_stepFrame = true;
        dt.stepPrevFrame();
        m_isPrevFramePlay = true;
    }
}





void Player::sliderSeek(double pos)
{
    dt.seek(pos);
}

void Player::timerEvent(QTimerEvent *e)
{
    if (isSliderPress)return;
    if (ui->ctrlbar->getSliderPress()) return ;
    long long total = dt.totalMs;
    if (total > 0)
    {
        ui->ctrlbar->setSliderValue(dt.getVideoPts());
    }
}

void Player::closeEvent(QCloseEvent *e)
{
    if (m_isClosing) {          // 已经清理过了，直接放行
        e->accept();
        return;
    }
    m_isClosing = true;
    // 防用户在退出过程中乱点
    ui->ctrlbar->setDisabled(true);
    ui->topMenu->setDisabled(true);
    // 先停播放、停线程（GUI 线程执行，安全）
    dt.close();
    // 接受关闭
    e->accept();
}

void Player::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        Qt::Edges edges = hitTestEdges(e->globalPosition().toPoint());
        if (edges) {
            beginResize(edges, e->globalPosition().toPoint());
            return;                         // 消费事件，开始缩放
        }
    }
    QWidget::mousePressEvent(e);
}

void Player::mouseMoveEvent(QMouseEvent *e)
{
    if (m_isResizing) {
        updateResize(e->globalPosition().toPoint());
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void Player::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_isResizing && e->button() == Qt::LeftButton) {
        m_isResizing = false;
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

bool Player::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            Qt::Edges edges = hitTestEdges(me->globalPosition().toPoint());
            if (edges) {
                beginResize(edges, me->globalPosition().toPoint());
                return true;                // 拦截：进入缩放
            }
        }
    }
    else if (event->type() == QEvent::MouseMove && m_isResizing) {
        updateResize(static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
        return true;
    }
    else if (event->type() == QEvent::MouseButtonRelease && m_isResizing) {
        m_isResizing = false;
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void Player::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape && m_isFullScreen) {
        toggleFullScreen();   // 退出全屏
        return;
    }
    QWidget::keyPressEvent(e);
}

void Player::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        // 检查是否都是本地文件
        bool allLocal = true;
        for (const QUrl &url : e->mimeData()->urls()) {
            if (!url.isLocalFile()) {
                allLocal = false;
                break; }
        }
        if (allLocal) {
            e->acceptProposedAction();
            return;
        }
    }
    e->ignore();
}

void Player::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    // 如果没在播放就取第一个文件播放，其余加入列表
    if(!m_isInit){
        QString first = urls.first().toLocalFile();
        playFile(first);
    }
    for (int i = 0; i < urls.size(); ++i) {
        QString path = urls[i].toLocalFile();
        addToPlayList(path);
    }
    e->acceptProposedAction();
}





Qt::Edges Player::hitTestEdges(const QPoint &globalPos) const
{
    if (isMaximized())
        return {};                          // 最大化时不允许缩放
    QPoint p = mapFromGlobal(globalPos);
    Qt::Edges edges = {};
    if (p.x() <= kBorder)                    edges |= Qt::LeftEdge;
    else if (p.x() >= width() - kBorder)     edges |= Qt::RightEdge;
    if (p.y() <= kBorder)                    edges |= Qt::TopEdge;
    else if (p.y() >= height() - kBorder)    edges |= Qt::BottomEdge;
    return edges;
}

void Player::beginResize(Qt::Edges edges, const QPoint &globalPos)
{
    m_isResizing  = true;
    m_resizeEdge  = edges;
    m_pressGlobal = globalPos;
    m_startGeo    = geometry();
    m_minSize     = minimumSize();
}

void Player::updateResize(const QPoint &globalPos)
{
    if (!m_isResizing)
        return;
    QPoint delta = globalPos - m_pressGlobal;
    QRect  g     = m_startGeo;
    // 按拖动的边调整几何
    if (m_resizeEdge & Qt::LeftEdge)   g.setLeft(g.left() + delta.x());
    if (m_resizeEdge & Qt::RightEdge)  g.setRight(g.right() + delta.x());
    if (m_resizeEdge & Qt::TopEdge)    g.setTop(g.top() + delta.y());
    if (m_resizeEdge & Qt::BottomEdge) g.setBottom(g.bottom() + delta.y());
    // 最小尺寸约束（setGeometry 不会自动遵守 minimumSize）
    if (m_resizeEdge & Qt::LeftEdge) {
        int maxLeft = m_startGeo.right() - m_minSize.width();
        if (g.left() > maxLeft) g.setLeft(maxLeft);
    }
    if (m_resizeEdge & Qt::TopEdge) {
        int maxTop = m_startGeo.bottom() - m_minSize.height();
        if (g.top() > maxTop) g.setTop(maxTop);
    }
    if (g.width()  < m_minSize.width())  g.setWidth(m_minSize.width());
    if (g.height() < m_minSize.height()) g.setHeight(m_minSize.height());
    setGeometry(g);
}

void Player::addToPlayList(const QString &path)
{
    ui->playList->addFile(path);
    dt.setHasPlayList(true);
}
