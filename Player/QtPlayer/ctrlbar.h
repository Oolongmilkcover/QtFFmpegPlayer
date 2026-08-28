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
public slots:
    void setPausePictrue(bool isPause);

    void setSliderValue(int value);
    int getSliderValue();

    void setSliderMaximum(int maximum);
    int getSliderMaximum() const;

    bool getSliderPress();

    void stepFrameTime(bool flag);

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
};

#endif // CTRLBAR_H
