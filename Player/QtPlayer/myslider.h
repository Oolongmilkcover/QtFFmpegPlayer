#ifndef MYSLIDER_H
#define MYSLIDER_H

#include <QSlider>

class mySlider : public QSlider
{
    Q_OBJECT
public:
    explicit mySlider(QWidget *parent = nullptr);
    void mousePressEvent(QMouseEvent *e);
signals:
};

#endif // MYSLIDER_H
