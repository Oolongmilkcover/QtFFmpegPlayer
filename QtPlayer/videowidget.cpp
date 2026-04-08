#include "videowidget.h"
#include <QPainter>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}



VideoWidget::VideoWidget(QWidget *parent)
    : QWidget(parent)
{
}

VideoWidget::~VideoWidget()
{
    if (sws) {
        sws_freeContext(sws);
        sws = nullptr;
    }
}

void VideoWidget::init(int w, int h)
{
    std::lock_guard<std::mutex> lock(mux);

    width = w;
    height = h;

    img = QImage(width, height, QImage::Format_RGB888);
}

void VideoWidget::setpaint(AVFrame *frame)
{
    if (!frame) return;

    std::lock_guard<std::mutex> lock(mux);

    if (!sws) {
        sws = sws_getContext(
            frame->width, frame->height, (AVPixelFormat)frame->format,
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, 0, 0, 0
            );
    }

    uint8_t *dst[] = { img.bits() };
    int dst_linesize[] = { static_cast<int>(img.bytesPerLine()) };

    sws_scale(
        sws,
        frame->data,
        frame->linesize,
        0,
        frame->height,
        dst,
        dst_linesize
        );

    av_frame_free(&frame);

    update();  // 触发 paintEvent
}

void VideoWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    std::lock_guard<std::mutex> lock(mux);

    if (!img.isNull()) {
        painter.drawImage(rect(), img);
    }
}
