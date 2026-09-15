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
#include <QGuiApplication>   // 只在 Wayland 才需要请系统接管窗口缩放
#include <QWindow>           // startSystemResize / startSystemMove

//快进/快退的“尾部”去抖窗口：停止按键多久后，把累积的目标补上。
//配合“前沿”（这一串按键的第一次立即生效）实现 B 站式手感：
//第一次按下马上有反馈，连按/长按只累积，静默 kSeekDebounceMs 后再补一次最终位置。
static constexpr int kSeekDebounceMs = 120;

Player::Player(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Player)
{
    ui->setupUi(this);
    //设置窗口图标&默认窗口标题
    this->setWindowIcon(QIcon(":/workBtnPNG/OPlayer.png"));
    this->setWindowTitle("OPlayer");
    //去掉标题和原生按钮
    this->setWindowFlags(Qt::FramelessWindowHint);
    // 设置VideoWidget大小策略为自动扩展
    ui->video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 初始就铺满整个窗口
    resize(1280+12, 720+ui->ctrlbar->height()+ui->topMenu->height()+12);              // 默认窗口大小（更友好）
    setMinimumSize(432+12, 243+ui->ctrlbar->height()+ui->topMenu->height()+12);       // 最小限制（16:9）
    ui->video->setFixedSize(1280, 720);

    //边缘缩放  给所有可能抢焦点的子控件装过滤器
    ui->video->installEventFilter(this);
    ui->ctrlbar->installEventFilter(this);
    ui->playList->getListWidget()->installEventFilter(this);

    ui->video->setGeometry(0, 0, width(), height());

    //---------------------------------------------------------------------------
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


    //---------------------------------------------------------------------------
    //底部控制按键组信号操作
    //开始与暂停按钮控制
    connect(ui->ctrlbar, &CtrlBar::play, this,&Player::play);
    connect(ui->ctrlbar, &CtrlBar::pause, this,&Player::pause);
    connect(&dt,&DemuxThread::needPause,this,&Player::pause);
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
    //倍速
    connect(ui->ctrlbar, &CtrlBar::speedChanged, this, &Player::changeSpeed);
    //滤镜切换
    connect(ui->ctrlbar, &CtrlBar::filterChanged, this, [this](int type){
        ui->video->setFilterType(type);
    });


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
    if (m_seekDebounceTimerId) killTimer(m_seekDebounceTimerId);
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
    // if(m_isInit){
    //    dt.setVolume(pos);
    // }
    dt.setVolume(pos);
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
    if(!m_isInit) return;
    exitStepFrame();
    setPausePicture(true);
    dt.close();
    ui->video->clearScreen();
    ui->ctrlbar->setDisabled(true);
    ui->topMenu->setNoPlayText();
    m_videoSrcH = 0;
    m_videoSrcW = 0;
    m_isInit = false;
    ui->video->clearScreen();
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

    if (path.isEmpty()) return;


    // 起点：进程内统一的纳秒时间戳
    //const qint64 startNs = perfNowNs();
    //qDebug() << "[PERF] playFile entry, startNs = " << startNs <<" ns";

    //换文件前先退出逐帧
    exitStepFrame();
    //this->setWindowTitle(path);
    if (!dt.openFile(path.toUtf8().constData(), ui->video)) {
        QMessageBox::information(0, "error", "open file failed!");
        return;
    }

    //ui->video->setPerfStartNs(startNs);

    emit setPlayingText(path);
    dt.start();
    this->showNormal();
    m_isInit = true;
    // 重置滑块状态，避免之前操作的影响
    isSliderPress = false;                 // 清除按下标志
    // 换文件时丢弃上一次遗留的待生效 seek 与去抖定时器
    if (m_seekDebounceTimerId) {
        killTimer(m_seekDebounceTimerId);
        m_seekDebounceTimerId = 0;
    }
    m_pendingSeekMs = -1;
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
    resizeEvent(nullptr);
    //setPausePicture(dt.getIsPause());
    if(!dt.getIsPause()){
        play();
    }else{
        pause();
    }
    if (!dt.hasVideo()) {
        ui->video->clearScreen();   // 纯音频：黑屏
    }
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
    queueSeekBy(+5000);
}

void Player::rewindSeekFiveSec()
{
    queueSeekBy(-5000);
}

// 快进/快退统一入口：前沿立即生效 + 尾部去抖收尾。
//   前沿：这一串按键的第一次立即 seek（消除“按了要等”的延迟感）
//   尾部：窗口内的后续按键只累积到 m_pendingSeekMs，静默 kSeekDebounceMs 后
//         由 timerEvent 补一次最终位置；只按一次时它是空的，收尾会自动跳过。
// 因此：单按 = 1 次 seek，连按/长按 = 2 次 seek（而不是 N 次）。
void Player::queueSeekBy(long long deltaMs)
{
    if(!m_isInit || dt.m_seekPauseing.load()) return;
    //逐帧中快进/快退：先退出逐帧（位置会对齐）
    exitStepFrame();

    const long long totalMs = dt.totalMs;
    if(totalMs <= 0) return;   //时长未知，没法和范围对齐

    //基准：有“已累积但还没生效”的目标就接着累积，否则取当前主时钟位置
    const long long base = (m_pendingSeekMs >= 0) ? m_pendingSeekMs : dt.pts.load();
    long long target = base + deltaMs;
    if(target < 0) target = 0;
    if(target > totalMs) target = totalMs;

    //进度条 + 时间文字立刻跟到目标（setSliderValue 内部会同步 playSlider 和 playTimeEdit）
    ui->ctrlbar->setSliderValue((int)target);

    if(m_seekDebounceTimerId == 0){
        //前沿：这一串按键的第一次立即生效
        m_pendingSeekMs = -1;
        dt.seekToMs(target);
    }else{
        //后续按键：只累积，等静默后再 seek 末次
        m_pendingSeekMs = target;
    }

    //尾部：每次按键都重置，静默 kSeekDebounceMs 后由 timerEvent 收尾
    if(m_seekDebounceTimerId) killTimer(m_seekDebounceTimerId);
    m_seekDebounceTimerId = startTimer(kSeekDebounceMs);
}



void Player::stepFrame(int mode)
{
    if(!m_isInit) return;
    //上一帧 / 下一帧：进入逐帧模式（暂停渲染，解码继续）
    bool success = false;
    if(mode == 1){
        success = dt.stepNextFrame();
    }else{
        success = dt.stepPrevFrame();
    }
    if(!success){
        //不支持逐帧（没有视频流 / 纯音频文件）时保持原状
        return;
    }
    if(!m_stepFrame){
        m_stepFrame = true;
        m_isPause = true;
        //按钮切换到暂停外观，并锁掉逐帧期间不该用的按钮
        ui->ctrlbar->setPausePictrue(true);
        ui->ctrlbar->stepFrameTime(true);
        ui->topMenu->stepFrameTime(true);
    }
}

void Player::exitStepFrame()
{
    if(!m_stepFrame) return;
    m_stepFrame = false;
    ui->ctrlbar->stepFrameTime(false);
    ui->topMenu->stepFrameTime(false);
    //让dt退出逐帧：恢复音频、把播放位置对齐到当前显示的帧
    dt.endFrameStep();
}

void Player::adjustVolume(double delta)
{
    if(!m_isInit) return;
    double vol = m_nowVolume + delta;
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;
    m_nowVolume = vol;
    setVolume(vol);   // 会同步 UI 滑块和 dt.setVolume
    // 同步 CtrlBar 音量滑块显示
    ui->ctrlbar->setVolumeSlider(vol * 100);   // 需要 CtrlBar 提供这个方法
}

void Player::changeSpeed(double delta)
{
    double newSpeed = m_speed + delta;
    if (newSpeed < 0.5) newSpeed = 0.5;
    if (newSpeed > 2.0)  newSpeed = 2.0;
    m_speed = newSpeed;
    dt.setSpeed(m_speed);
    ui->ctrlbar->setSpeedLabel(m_speed);
}






void Player::sliderSeek(double pos)
{
    if(!m_isInit) return;
    //用户主动拖动：丢弃还没生效的累积目标，免得它晚一步把进度拽回去
    if(m_seekDebounceTimerId){
        killTimer(m_seekDebounceTimerId);
        m_seekDebounceTimerId = 0;
    }
    m_pendingSeekMs = -1;
    //拖动进度条也退出逐帧
    exitStepFrame();
    // // -----------
    // static int times = 0;
    // qDebug()<<"这是第"<<++times<<"次seek";
    // qint64 startNs = perfNowNs();
    // ui->video->setPerfStartNs(startNs);
    // ui->video->m_needNsDiff.store(true);
    // //-----------
    dt.seek(pos);
}

void Player::timerEvent(QTimerEvent *e)
{
    //去抖定时器到点 = 用户已经停止按键 → 现在真正 seek 一次
    if (m_seekDebounceTimerId && e->timerId() == m_seekDebounceTimerId)
    {
        killTimer(m_seekDebounceTimerId);
        m_seekDebounceTimerId = 0;
        const long long target = m_pendingSeekMs;
        m_pendingSeekMs = -1;
        //用户此刻正在拖动进度条：让拖动接管，丢弃累积目标，
        //否则会在拖动过程中先跳一次（随后松手又会按拖动位置再跳一次）
        if (ui->ctrlbar->getSliderPress()) return;
        if (target >= 0) dt.seekToMs(target);
        return;
    }

    if (isSliderPress)return;
    if (ui->ctrlbar->getSliderPress()) return ;
    long long total = dt.totalMs;
    if (total > 0)
    {
        //有待生效目标时，进度条显示目标（跟着按键走），而不是当前播放位置
        const long long showMs = (m_pendingSeekMs >= 0) ? m_pendingSeekMs : dt.pts.load();
        ui->ctrlbar->setSliderValue((int)showMs);
    }
}

void Player::closeEvent(QCloseEvent *e)
{
    if (m_isClosing) {          // 已经清理过了，直接放行
        e->accept();
        return;
    }
    m_isClosing = true;
    // 丢掉还没生效的 seek，避免退出过程中再触发一次
    if (m_seekDebounceTimerId) {
        killTimer(m_seekDebounceTimerId);
        m_seekDebounceTimerId = 0;
    }
    m_pendingSeekMs = -1;
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
    if(!m_isInit) return;
    int key = e->key();
    switch(key){
    case Qt::Key_Space:
        if(m_isPause){
            play();
        }else{
            pause();
        }
        return;
    case Qt::Key_Escape:
        if (m_isFullScreen) toggleFullScreen();
        return;
    case Qt::Key_Right:   // 快进
        ffSeekFiveSec();
        return;
    case Qt::Key_Left:    // 快退
        rewindSeekFiveSec();
        return;
    case Qt::Key_Up:      // 音量+
        adjustVolume(+0.05);
        return;
    case Qt::Key_Down:    // 音量-
        adjustVolume(-0.05);
        return;
    case Qt::Key_X:       // 降速
        changeSpeed(-0.1);
        return;
    case Qt::Key_C:       // 升速
        changeSpeed(0.1);
        return;
    case Qt::Key_F:       // 下一帧
        stepFrame(1);
        return;
    case Qt::Key_D:       // 上一帧
        stepFrame(2);
        return;
    default:
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
    /*
     * 只有 Wayland 需要请系统接管缩放：客户端在那里不能自己改顶层窗口几何
     * （setGeometry 会被合成器忽略，表现就是"边缘拖不动"），只能请求交互式缩放。
     *
     * X11 / Windows 保持自绘缩放（和 v2.1 一致）：它们本来就允许客户端改几何，
     * 而且自绘走正常事件循环；原生缩放是模态循环，期间 Qt 不跑事件循环，
     * 拖窗口时画面/播放会停住，看起来就是卡顿。
     * 和 startSystemMove 一样，必须在鼠标按下的处理里同步调用。
     */
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        QWindow *wh = windowHandle();
        if (wh && wh->startSystemResize(edges)) {
            m_isResizing = false;       // 交给合成器，不再走自绘缩放
            return;
        }
    }

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

void Player::play()
{
    if(!m_isInit) return;
    //如果在逐帧，先退出逐帧（位置会对齐）
    exitStepFrame();
    ui->ctrlbar->stepFrameTime(false);
    ui->ctrlbar->setPausePictrue(false);
    ui->topMenu->stepFrameTime(false);
    m_stepFrame = false;
    m_isPause = false;
    dt.setPause(false);
}

void Player::pause()
{
    if(!m_isInit) return;
    //如果在逐帧，先退出逐帧（位置会对齐）
    exitStepFrame();
    ui->ctrlbar->setPausePictrue(true);
    m_isPause = true;
    dt.setPause(true);
}
