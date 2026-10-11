#ifndef CTRLBAR_H
#define CTRLBAR_H

#include <QWidget>

namespace Ui {
class CtrlBar;
}

class CtrlBar : public QWidget
{
    Q_OBJECT

public:
    explicit CtrlBar(QWidget *parent = nullptr);
    ~CtrlBar();

private:
    void sliderPress();
    void sliderRelease();

    void volumeSliderPress();
    void volumeSliderRelease();
signals:
    void play();
    void pause();
    void seek(double pos);
    void setVolume(double pos);
    void stop();
    void fullPlay();
    void showOrHidePlayList();
    void next();
    void prev();
    void ff();
    void rewind();
    void prevFrame(int mode);
    void nextFrame(int mode);
    void filterChanged(int type);
    void speedChanged(double speed);
public slots:
    void setPausePictrue(bool isPause);

    void setSliderValue(int value);
    int getSliderValue();

    void setSliderMaximum(int maximum);
    int getSliderMaximum() const;

    bool getSliderPress();

    void stepFrameTime(bool flag);

    void setSpeedLabel(double speed);
    void setVolumeSlider(int value);   // 0~100

    /*
     * 流媒体模式：快进 / 快退 / 逐帧 / 倍速 / 上下集对直播流没有意义，
     * 把这些按钮和进度条置灰（播放/暂停、停止、音量、全屏、滤镜、播放列表照旧可用）。
     * Player::playFile 每次打开都按 DemuxThread::isStreamMedia() 调一次，
     * 换回本地视频时自动恢复。
     */
    void setStreamMode(bool isStream);

protected:
    void resizeEvent(QResizeEvent *event) override;


private slots:
    void on_playOrPauseBtn_clicked();

    void on_stopBtn_clicked();

    void on_prevBtn_clicked();

    void on_nextBtn_clicked();

    void on_slowdownBtn_clicked();

    void on_speedupBtn_clicked();

    void on_playListBtn_clicked();

    void on_settingBtn_clicked();

    QString msToString(int ms);

    void on_fullPlayBtn_clicked();

    void on_ffBtn_clicked();

    void on_rewindBtn_clicked();

    void on_prevFrameBtn_clicked();

    void on_nextFrameBtn_clicked();

private:
    Ui::CtrlBar *ui;
    bool m_isPause = false;
    bool m_isSliderPress = false;
    bool m_isVolumeSliderPress = false;
    bool m_isSliderInit = false;
    bool m_isPlayLiseShow = false;
    //是否处于流媒体模式（决定 stepFrameTime 退出逐帧时能不能把按钮重新点亮）
    bool m_isStreamMode = false;
    double m_speed = 1.0;
};

#endif // CTRLBAR_H
