#pragma once

#include <QObject>
#include <QMouseEvent>
#include <QSlider>
class Slider : public QSlider
{
    Q_OBJECT

public:
    Slider(QWidget *parent=NULL);
    ~Slider();
    void mousePressEvent(QMouseEvent *e);
};
