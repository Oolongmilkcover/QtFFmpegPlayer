#ifndef MYSLIDER_H
#define MYSLIDER_H

#include <QSlider>

class mySlider : public QSlider
{
    Q_OBJECT
public:
    explicit mySlider(QWidget *parent = nullptr);
    void mousePressEvent(QMouseEvent *e);
    void mouseMoveEvent(QMouseEvent *e);
    void mouseReleaseEvent(QMouseEvent *e);

signals:

private:
    bool m_isDragging = false;
};

#endif // MYSLIDER_H
