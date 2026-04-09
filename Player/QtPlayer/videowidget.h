#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

#include <QWidget>
#include <QImage>
#include <mutex>

struct AVFrame;
class SwsContext;
class VideoWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr);
    ~VideoWidget();

    void init(int w, int h);
    void setpaint(AVFrame *frame);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::mutex mux;
    QImage img;
    SwsContext *sws = nullptr;
    int width = 0;
    int height = 0;
};

#endif
