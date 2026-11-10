#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <QListWidget>
#include <QWidget>

namespace Ui {
class PlayList;
}

class PlayList : public QWidget
{
    Q_OBJECT

public:
    explicit PlayList(QWidget *parent = nullptr);
    ~PlayList();

signals:
    void noMoreToPlay();
    void playThis(QListWidgetItem* item);
public:
    void addFile(const QString &path);

    QListWidget* getListWidget() const ;
    // 当前选中行
    int  currentRow() const;
    // 选中某行
    void setCurrentRow(int row);
    // 删除当前项
    void removeCurrentItem();
    // 列表数量
    int  count() const;
    // 取某行完整路径
    QString itemPath(int row) const;
    // 选下一行（环形）
    void next();
    // 选上一行（环形）
    void prev();

protected:
    void contextMenuEvent(QContextMenuEvent *e) override;

    void keyPressEvent(QKeyEvent *e) override;

private:
    Ui::PlayList *ui;
};

#endif // PLAYLIST_H
