#include "myslider.h"

#include <QMouseEvent>

mySlider::mySlider(QWidget *parent)
    :QSlider(parent)
{}

// void mySlider::mousePressEvent(QMouseEvent *e)
// {
//     double pos = (double)e->pos().x() / (double)width();
//     setValue(pos * this->maximum());
//     // 发射按下信号
//     emit sliderReleased();
// }

void mySlider::mousePressEvent(QMouseEvent *e)
{
    if(e->button() == Qt::RightButton){
        return;
    }
    // 先调用父类，以保持基本行为（比如获得焦点等）
    QSlider::mousePressEvent(e);
    // 计算点击位置对应的值
    double pos = (double)e->pos().x() / (double)width();
    int value = pos * maximum();
    setValue(value);
    m_isDragging = true;
    // 发射按下信号
    emit sliderPressed();
}
void mySlider::mouseMoveEvent(QMouseEvent *e)
{
    if (m_isDragging&& (e->buttons() & Qt::LeftButton)) {
        double pos = (double)e->pos().x() / (double)width();
        pos = qBound(0.0, pos, 1.0);
        int value = pos * maximum();
        setValue(value);
        // 如果需要实时预览，可以发射 sliderMoved
        emit sliderMoved(value);
    }
    QSlider::mouseMoveEvent(e); // 可选，调用父类
}

void mySlider::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_isDragging) {
        m_isDragging = false;
        emit sliderReleased();
    }
    QSlider::mouseReleaseEvent(e);
}

