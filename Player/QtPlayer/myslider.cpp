#include "myslider.h"

#include <QMouseEvent>

mySlider::mySlider(QWidget *parent)
    :QSlider(parent)
{}

void mySlider::mousePressEvent(QMouseEvent *e)
{
    double pos = (double)e->pos().x() / (double)width();
    setValue(pos * this->maximum());
    //原有事件处理
    emit sliderReleased();
}
