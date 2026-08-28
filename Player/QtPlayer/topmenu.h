#ifndef TOPMENU_H
#define TOPMENU_H
#include <QPoint>
#include <QWidget>

namespace Ui {
class TopMenu;
}

class TopMenu : public QWidget
{
    Q_OBJECT

public:
    explicit TopMenu(QWidget *parent = nullptr);
    ~TopMenu();

signals:
    void openFile();
    void closeClicked();
    void hideWindow();
    void maximization();
    void restore();
public slots:
    void setPlayingText(QString name);

    QString getPlayFileName(const QString& filePath);

    void setNoPlayText();

    void maxOrRestoreChange();

    void stepFrameTime(bool flag);
protected:
    // 按下、移动、释放 → 拖动顶层窗口
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

    void resizeEvent(QResizeEvent * e) override;

private slots:
    void on_openFileBtn_clicked();

    void on_closeBtn_clicked();

    void on_fullOrBackBtn_clicked();

    void on_hideWindowBtn_clicked();



private:
    Ui::TopMenu *ui;
    bool m_isWindowMax = false;
    bool  m_isDragging = false;   // 是否正在拖动
    QPoint m_dragOffset;          // 按下点相对窗口左上角的偏移
};

#endif // TOPMENU_H
